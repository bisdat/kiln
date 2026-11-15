// Smoke (browser mock): the beta channel (dev.29). Settings > Updates names the channels Release and Beta (test builds);
// choosing the other one asks first (worlds, multiplayer), installs that build now and restarts, both ways: to a test
// build and back to the release, with the "before beta" copy put back when asked. A test build shows "Beta <n>" in the
// header (one click back) and on the Deck; the Nexus build only says where test builds are.
// window.__kilnUpdateMock drives it (src/api/mockUpdate.ts: beta(), channelOffer(), switches()). SHOTS=0 skips shots.
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";
import { join } from "node:path";

const SHOTS = process.env.SHOTS_DIR || join(import.meta.dirname, "..", "..", ".out", "launcher", "shots");
const DOWNLOAD_PAGE = "https://frostdev.io/kiln/download";

async function open(page: Page, route = "/", query = "") {
  await page.goto(`/${query}#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

async function shot(page: Page, name: string) {
  if (process.env.SHOTS === "0") return;
  mkdirSync(SHOTS, { recursive: true });
  await page.waitForTimeout(400);
  await page.screenshot({ path: join(SHOTS, `mock-${name}.png`) });
}

const mock = (page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(([f, a]) => (window as unknown as { __kilnUpdateMock: Record<string, (...x: unknown[]) => unknown> }).__kilnUpdateMock[f as string](...(a as unknown[])), [fn, args] as const);
const switches = (page: Page) => mock(page, "switches") as Promise<{ channel: string; restore: boolean }[]>;
const opened = (page: Page) => page.evaluate(() => (window as unknown as { __kilnMock: { opened: () => string[] } }).__kilnMock.opened());
const updatesCard = (page: Page) => page.locator(".k-card", { has: page.getByRole("heading", { name: "Updates" }) });

test("Settings names the channels for players: Release first, then Beta (test builds)", async ({ page }) => {
  await open(page, "/settings");
  const select = updatesCard(page).getByLabel("Channel");
  await expect(select).toHaveValue("dev");
  await expect(select.locator("option")).toHaveText(["Release", "Beta (test builds)"]);
  await expect(page.getByTestId("beta-mark")).toHaveCount(0);
});

test("to the beta and back: one click each way, worlds and servers said first", async ({ page }) => {
  await open(page, "/settings");
  const card = updatesCard(page);
  await card.getByLabel("Channel").selectOption("beta");
  const ask = page.getByRole("dialog", { name: "Switch to the beta?" });
  await expect(ask).toBeVisible();
  await expect(ask).toContainText("Test builds of the next release");
  await expect(ask).toContainText("may not load the same way on the release");
  await expect(ask).toContainText("servers and players must run the same build");
  await expect(ask).not.toContainText(/pre-?release|channel/i);
  await shot(page, "beta-switch-ask");
  // Cancel changes nothing
  await ask.getByRole("button", { name: "Cancel" }).click();
  await expect(ask).toHaveCount(0);
  await expect(card.getByLabel("Channel")).toHaveValue("dev");
  expect(await switches(page)).toEqual([]);

  await card.getByLabel("Channel").selectOption("beta");
  await page.getByRole("dialog", { name: "Switch to the beta?" }).getByRole("button", { name: "Switch now" }).click();
  const ready = page.getByRole("dialog", { name: "Ready: 0.2.0-dev.43 beta 1" });
  await expect(ready).toBeVisible({ timeout: 8000 });
  expect(await switches(page)).toEqual([{ channel: "beta", restore: false }]);
  await expect(card.getByLabel("Channel")).toHaveValue("beta");
  await shot(page, "beta-switch-ready");
  await ready.getByRole("button", { name: "Restart now" }).click();

  // the test build runs: the header says Beta 1, Settings says which build and offers the way back
  const mark = page.getByTestId("beta-mark");
  await expect(mark).toHaveText("Beta 1");
  await expect(card.getByTestId("beta-build")).toContainText("Test build 1 of 0.2.0-dev.43");
  await expect(page.getByRole("dialog", { name: "Updated to 0.2.0-dev.43 beta 1" })).toBeVisible();
  await shot(page, "beta-running");
  await page.getByRole("dialog", { name: "Updated to 0.2.0-dev.43 beta 1" }).getByRole("button", { name: "Dismiss" }).click();

  // back: the mark opens the switch back, which goes down to the release's newest
  await mock(page, "set", { beforeBeta: { taken: "2026-11-15T10:00:00Z", from: "0.2.0-dev.42", by: "0.2.0-dev.43+beta.1", files: ["settings.json"] } });
  await mark.click();
  const back = page.getByRole("dialog", { name: "Switch back to the release?" });
  await expect(back).toContainText("also when it is older than this test build");
  await expect(back).toContainText("This is test build 1 of 0.2.0-dev.43");
  const restore = back.getByRole("switch", { name: /Put back my profiles and settings from before the beta/ });
  await expect(restore).not.toBeChecked();
  await expect(back).toContainText("Mods you added in the beta stay in the Library");
  await back.locator("label.k-toggle").click();
  await expect(restore).toBeChecked();
  await shot(page, "beta-switch-back-ask");
  await back.getByRole("button", { name: "Switch back" }).click();
  const readyBack = page.getByRole("dialog", { name: "Ready: 0.2.0-dev.41" });
  await expect(readyBack).toBeVisible({ timeout: 8000 });
  await expect(readyBack).toContainText("your profiles and settings from before the beta go back first");
  expect((await switches(page)).at(-1)).toEqual({ channel: "dev", restore: true });
  await readyBack.getByRole("button", { name: "Restart now" }).click();
  await expect(mark).toHaveCount(0);
  await expect(card.getByLabel("Channel")).toHaveValue("dev");
  await expect(card.getByTestId("beta-build")).toHaveCount(0);
});

test("a switch with nothing to switch to says so and installs nothing", async ({ page }) => {
  await open(page, "/settings");
  await mock(page, "channelOffer", "beta", null);
  await updatesCard(page).getByLabel("Channel").selectOption("beta");
  await page.getByRole("dialog", { name: "Switch to the beta?" }).getByRole("button", { name: "Switch now" }).click();
  await expect(page.getByText("Nothing to switch to")).toBeVisible();
  await expect(page.getByTestId("beta-mark")).toHaveCount(0);
});

test("an install that cannot replace itself gets the download page", async ({ page }) => {
  await open(page, "/settings");
  await mock(page, "set", { install: { kind: "deb", inPlace: false, reason: "Installed from a .deb package: your package manager updates it." } });
  await updatesCard(page).getByLabel("Channel").selectOption("beta");
  const ask = page.getByRole("dialog", { name: "Switch to the beta?" });
  await expect(ask).toContainText("Installed from a .deb package");
  await ask.getByRole("button", { name: "Open the download page" }).click();
  await expect.poll(() => opened(page)).toEqual(["https://frostdev.io/kiln"]);
  expect(await switches(page)).toEqual([]);
});

test.describe("Deck mode", () => {
  test.use({ viewport: { width: 1280, height: 800 } });

  test("Deck Settings: Channel, the switch both ways, the Beta mark under the logo", async ({ page }) => {
    await page.goto("/?deck=steamdeck#/deck/settings");
    const row = page.locator('[data-deck-id="set-channel"]');
    await expect(row).toContainText("Channel");
    await expect(row).toContainText("Release");
    await expect(page.getByTestId("deck-beta-mark")).toHaveCount(0);
    await row.click();
    const ask = page.locator('[data-testid="deck-switch-to-beta"]');
    await expect(ask).toContainText("Test builds of the next release");
    await expect(ask).toContainText("servers and players must run the same build");
    await page.getByRole("dialog", { name: "Switch to the beta?" }).getByRole("button", { name: "Switch now" }).click();
    const ready = page.getByRole("dialog", { name: "Ready: 0.2.0-dev.43 beta 1" });
    await expect(ready).toBeVisible({ timeout: 8000 });
    await ready.getByRole("button", { name: "Restart now" }).click();
    await expect(page.getByTestId("deck-beta-mark")).toHaveText("Beta 1");
    await expect(row).toContainText("Test build 1 of 0.2.0-dev.43");
    await expect(row).toHaveAttribute("data-deck-a", "Switch back");
    await shot(page, "beta-deck-settings");
    await mock(page, "set", { beforeBeta: { taken: "2026-11-15T10:00:00Z", from: "0.2.0-dev.42", by: "0.2.0-dev.43+beta.1", files: ["settings.json"] } });
    await row.click();
    const back = page.getByRole("dialog", { name: "Switch back to the release?" });
    await expect(back).toContainText("Mods you added in the beta stay in the Library");
    await back.getByRole("button", { name: "Switch back, put back my setup" }).click();
    await expect(page.getByRole("dialog", { name: "Ready: 0.2.0-dev.41" })).toBeVisible({ timeout: 8000 });
    expect((await switches(page)).at(-1)).toEqual({ channel: "dev", restore: true });
    await page.getByRole("dialog", { name: "Ready: 0.2.0-dev.41" }).getByRole("button", { name: "Restart now" }).click();
    await expect(page.getByTestId("deck-beta-mark")).toHaveCount(0);
  });

  test("Nexus build: Deck Settings says where test builds are and switches nothing", async ({ page }) => {
    await page.goto("/?dist=nexus&deck=steamdeck#/deck/settings");
    await expect(page.locator('[data-deck-id="set-channel"]')).toHaveCount(0);
    const row = page.locator('[data-deck-id="set-test-builds"]');
    await expect(row).toContainText("Test builds");
    await row.click();
    await expect.poll(() => opened(page)).toEqual([DOWNLOAD_PAGE]);
    await expect(page.getByTestId("deck-beta-mark")).toHaveCount(0);
  });
});

test("Nexus build: Settings says where test builds are, with no channel and no switch", async ({ page }) => {
  await open(page, "/settings", "?dist=nexus");
  const card = updatesCard(page);
  await expect(card.getByLabel("Channel")).toHaveCount(0);
  await expect(card.getByTestId("nexus-beta")).toContainText("Test builds (beta)");
  await card.getByTestId("nexus-test-builds").click();
  await expect.poll(() => opened(page)).toEqual([DOWNLOAD_PAGE]);
  await expect(page.getByTestId("beta-mark")).toHaveCount(0);
});

