// "Come back after installing Kiln" (DESIGN 1.3) in mock mode: a pack code the download carried opens its preview at
// start; at the very first start a pack link the pack page put on the clipboard does; any other start, or a clipboard
// holding something else, opens nothing. Nothing installs without the preview's own clicks.
import { expect, test, type Page } from "@playwright/test";

async function open(page: Page, url: string, clipboard: string | null = null) {
  await page.addInitScript((text) => {
    Object.defineProperty(navigator, "clipboard", { configurable: true, value: { readText: async () => text ?? "", writeText: async () => {} } });
  }, clipboard);
  await page.goto(url);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

test("the code the download carried opens its pack at start", async ({ page }) => {
  await open(page, "/?mock=1&crucible=resume#/");
  await expect(page.getByTestId("crucible-resumed")).toHaveText("Kiln is ready. Here is Cinder Trails, the pack you came for.");
  await expect(page.getByTestId("crucible-install")).toBeDisabled();
  const cmds = await page.evaluate(() => (window as unknown as { __kilnCrucibleMock: { requests: () => { cmd: string }[] } }).__kilnCrucibleMock.requests().map((r) => r.cmd));
  expect(cmds).not.toContain("crucible_install");
});

test("the first start takes the pack link the pack page copied", async ({ page }) => {
  await open(page, "/?mock=1&crucible=first#/", "https://frostdev.io/kiln/crucible/p/M2PD-7FJC");
  await expect(page.getByTestId("crucible-resumed")).toContainText("Frost Hollow");
});

test("the first start with other text on the clipboard opens nothing", async ({ page }) => {
  await open(page, "/?mock=1&crucible=first#/", "a shopping list");
  await expect(page.getByTestId("crucible-share")).toBeVisible();
  await expect(page.getByTestId("crucible-preview")).toHaveCount(0);
});

test("a later start ignores the clipboard", async ({ page }) => {
  await open(page, "/?mock=1#/", "https://frostdev.io/kiln/crucible/p/M2PD-7FJC");
  await expect(page.getByTestId("crucible-share")).toBeVisible();
  await expect(page.getByTestId("crucible-preview")).toHaveCount(0);
});
