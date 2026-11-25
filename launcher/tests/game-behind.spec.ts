// (0.2.0-dev.31.12) Play's Update game banner (browser mock; pages/PlayPage.tsx GameBehindBanner, shell/updateApply.ts
// gameBehind): when the game's Kiln is older than the one this launcher has, Play says both versions in a banner with
// Update game, which runs the update notice's own install (shell/runtimeChange.ts updateGame). Held while the game runs,
// gone when the game's Kiln is equal or newer and after the install by itself, and a failed install is said on it.
// HTR's feedback on 31.9: after a launcher update only a small orange "update ready" said the game's Kiln was behind.
// SHOTS_DIR: where the screenshots go (SHOTS=0 skips them).
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";
import { join } from "node:path";

const SHOTS = process.env.SHOTS_DIR || join(import.meta.dirname, "..", "..", ".out", "launcher", "shots");
const HAS = "0.2.0-dev+20261104.0f1ffa6"; // the mock game's Kiln
const NEXT = "0.2.1-dev+20261120.beef001"; // a newer bundle, imported by the test
const BETA = "0.2.2-dev+20261122.cafe002"; // a test build's Kiln still in the Library, newer than the launcher's
const SENTENCE = "This game has Kiln 0.2.0-dev (20261104); this launcher has Kiln 0.2.1-dev (20261120).";

async function open(page: Page, query = "") {
  await page.goto(`/${query}#/`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}
async function shot(page: Page, name: string) {
  if (process.env.SHOTS === "0") return;
  mkdirSync(SHOTS, { recursive: true });
  await page.waitForTimeout(300);
  await page.screenshot({ path: join(SHOTS, `mock-${name}.png`) });
}
const game = (page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(([f, a]) => (window as unknown as { __kilnMock: Record<string, (...x: unknown[]) => unknown> }).__kilnMock[f as string](...(a as unknown[])), [fn, args] as const);
const upd = (page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(([f, a]) => (window as unknown as { __kilnUpdateMock: Record<string, (...x: unknown[]) => unknown> }).__kilnUpdateMock[f as string](...(a as unknown[])), [fn, args] as const);
const importBundle = (page: Page, version: string) =>
  page.evaluate(async (n) => { await (await import("/src/api/commands.ts")).runtimeImport(`/home/player/Downloads/kiln-runtime-${n}.zip`); }, version);
/** The launcher has a newer Kiln than the game: the bundle in its Library and the game's status offering it. */
async function behind(page: Page, shipped = false) {
  await importBundle(page, NEXT);
  if (shipped) await upd(page, "set", { shippedRuntime: NEXT });
  await game(page, "setUpgradeAvailable", NEXT);
}
const banner = (page: Page) => page.locator(".k-banner", { has: page.getByTestId("game-behind") });
const button = (page: Page) => banner(page).getByRole("button", { name: "Update game" });

test("behind: the banner says both Kiln versions, and Update game installs the launcher's", async ({ page }) => {
  await open(page);
  await expect(page.getByTestId("game-behind")).toHaveCount(0);
  await behind(page);
  await expect(page.getByTestId("game-behind")).toHaveText(SENTENCE);
  await expect(banner(page).locator(".k-banner-title")).toHaveText("Update the game's Kiln");
  // the small note next to the Kiln fact stays
  await expect(page.getByRole("button", { name: "update ready" })).toBeVisible();
  await shot(page, "play-game-behind");
  await button(page).click();
  await expect(page.locator(".k-toast", { hasText: `Kiln ${NEXT} installed` })).toBeVisible({ timeout: 15_000 });
  await expect(banner(page)).toHaveCount(0);
  expect(await game(page, "installs")).toEqual([NEXT]);
});

test("held while the game runs: Update game is off and says Close the game first", async ({ page }) => {
  await open(page);
  await behind(page);
  await page.locator(".k-door").click();
  await expect(page.locator(".k-head-state")).toHaveText(/^Running/, { timeout: 8000 });
  await expect(button(page)).toBeDisabled();
  await expect(button(page)).toHaveAttribute("title", "Close the game first");
  await expect(banner(page).getByTestId("game-behind-why")).toHaveText("Close the game first");
  await shot(page, "play-game-behind-running");
  await game(page, "stopGame");
  await expect(button(page)).toBeEnabled({ timeout: 8000 });
  await expect(banner(page).getByTestId("game-behind-why")).toHaveCount(0);
  expect(await game(page, "installs")).toEqual([]);
});

test("hidden when the game's Kiln is equal or newer (no offer)", async ({ page }) => {
  await open(page);
  await behind(page);
  await expect(banner(page)).toBeVisible();
  await game(page, "setUpgradeAvailable", null);
  await expect(banner(page)).toHaveCount(0);
  await game(page, "setUpgradeAvailable", HAS); // the same version: never offered, never said
  await page.waitForTimeout(500);
  await expect(banner(page)).toHaveCount(0);
});

test("after a launcher update the Kiln it ships goes in by itself and the banner goes", async ({ page }) => {
  await open(page);
  await behind(page, true);
  await expect(banner(page)).toBeVisible();
  await upd(page, "justUpdated", "0.2.0-dev.41");
  await expect(page.locator(".k-toast", { hasText: `Kiln ${NEXT} installed` })).toContainText("It came with the launcher update to 0.2.0-dev.42.", { timeout: 15_000 });
  await expect(banner(page)).toHaveCount(0);
  expect(await game(page, "installs")).toEqual([NEXT]);
});

test("a failed install by itself: the banner stays with the reason, in one strip, and Update game tries again", async ({ page }) => {
  await open(page);
  await game(page, "failNextInstall", "No space left on device");
  await behind(page, true);
  await upd(page, "justUpdated", "0.2.0-dev.41");
  const failed = banner(page).getByTestId("game-behind-failed");
  await expect(failed).toContainText("No space left on device", { timeout: 15_000 });
  await expect(failed).toContainText(`The game still has Kiln ${HAS}.`);
  await expect(page.getByTestId("game-behind")).toHaveText(SENTENCE);
  // the failure is said once: no second "Update to ... failed" strip
  await expect(page.locator(".k-banner", { hasText: `Update to ${NEXT} failed` })).toHaveCount(0);
  await expect(button(page)).toBeEnabled();
  await page.getByRole("dialog", { name: "Updated to 0.2.0-dev.42" }).getByRole("button", { name: "Later" }).click();
  await shot(page, "play-game-behind-failed");
  expect(await game(page, "installs")).toEqual([NEXT]);
  await button(page).click();
  await expect(banner(page)).toHaveCount(0, { timeout: 15_000 });
  expect(await game(page, "installs")).toEqual([NEXT, NEXT]);
});

test("the Nexus build (no self-update) says it too and installs the Kiln it ships", async ({ page }) => {
  await open(page, "?dist=nexus");
  await behind(page);
  await expect(page.getByTestId("game-behind")).toHaveText(SENTENCE);
  await button(page).click();
  await expect(banner(page)).toHaveCount(0, { timeout: 15_000 });
  expect(await game(page, "installs")).toEqual([NEXT]);
});

test("no banner after a switch back (its own line) nor in a test build of the launcher (its channel notice)", async ({ page }) => {
  await open(page);
  await game(page, "setSwitchBack", "0.2.1-dev+20261115.b3e7a11", HAS);
  await expect(page.getByTestId("channel-notice")).toBeVisible();
  await expect(banner(page)).toHaveCount(0);
  await game(page, "setSwitchBack", null, null);
  await upd(page, "beta", 2);
  await behind(page);
  await expect(page.getByTestId("channel-notice")).toContainText("This launcher is a test build");
  await page.waitForTimeout(500);
  await expect(banner(page)).toHaveCount(0);
});

test("after its install the banner offers no other Kiln of the Library: the backend's next status says what is on offer", async ({ page }) => {
  await open(page);
  // a test build's newer Kiln left in the Library (after a switch back): a release build never offers it
  // (src-tauri state.rs runtime_policy), so Play must not offer it either while the fresh status is on its way
  await importBundle(page, BETA);
  await behind(page);
  await expect(page.getByTestId("game-behind")).toHaveText(SENTENCE);
  // the statuses after the install answer late, as the backend's full check of the game folder does
  for (let i = 0; i < 12; i++) await game(page, "planCommand", "game_status", { gate: "fresh-status" });
  await button(page).click();
  await expect(page.locator(".k-toast", { hasText: `Kiln ${NEXT} installed` })).toBeVisible({ timeout: 15_000 });
  await page.waitForTimeout(500);
  await expect(banner(page)).toHaveCount(0);
  await expect(page.getByRole("button", { name: "update ready" })).toHaveCount(0);
  await game(page, "openGate", "fresh-status");
  expect(await game(page, "installs")).toEqual([NEXT]);
});
