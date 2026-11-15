import { expect, test } from "@playwright/test";
type Mock = { __kilnMock: { scriptStoppedStart: (s: string) => void }; __kilnPlay: (mode: string) => void };
for (const deck of [false, true]) {
  test.describe(deck ? "Deck stopped start" : "Desktop stopped start", () => {
    test.beforeEach(async ({ page }) => {
      await page.setViewportSize(deck ? { width: 1280, height: 800 } : { width: 1024, height: 640 });
      await page.goto(`/${deck ? "?deck=steamdeck" : ""}#${deck ? "/deck" : "/"}`);
      await page.locator(".k-splash").waitFor({ state: "detached" });
    });
    for (const kind of ["roster", "boot-mod"]) test(`${kind}: named notice, filter, dismiss and reload`, async ({ page }) => {
      await page.evaluate((s) => (window as unknown as Mock).__kilnMock.scriptStoppedStart(s), kind);
      const play = page.locator(deck ? ".deck-mouth" : ".k-door");
      await expect(play).toBeEnabled();
      await play.click();
      const notice = page.getByRole("region", { name: "Kiln stopped the start", exact: true });
      await expect(notice).toBeVisible({ timeout: 30_000 });
      await expect(notice).toContainText("New Items");
      await expect(notice).toContainText("Big Stacks");
      await expect(notice).toContainText("Switch one of these mods off");
      await notice.getByRole("button", { name: "Review these mods" }).click();
      await expect(page).toHaveURL(/filter=/);
      if (deck) await expect(page.getByRole("list", { name: "Mods", exact: true }).getByRole("button")).toHaveCount(2);
      else await expect(page.getByRole("textbox", { name: "Filter", exact: true })).toHaveValue("ids:new-items,big-stacks");
      await page.goto(`/${deck ? "?deck=steamdeck" : ""}#${deck ? "/deck" : "/"}`);
      await expect(notice).toBeVisible();
      await notice.getByRole("button", { name: "Dismiss", exact: true }).click();
      await expect(notice).toHaveCount(0);
      await page.reload();
      await page.locator(".k-splash").waitFor({ state: "detached" });
      await expect(notice).toHaveCount(0);
    });
    for (const kind of ["unknown", "garbled", "stale", "quit"]) test(`${kind}: no stop notice at the end`, async ({ page }) => {
      await page.evaluate((s) => (window as unknown as Mock).__kilnMock.scriptStoppedStart(s), kind);
      await page.locator(deck ? ".deck-mouth" : ".k-door").click();
      // Observe a durable completion counter, so this assertion cannot pass before the scripted exit happened.
      await expect.poll(() => page.evaluate(() => (window as unknown as { __kilnStopCompleted?: number }).__kilnStopCompleted ?? 0)).toBe(1);
      await expect(page.getByRole("region", { name: "Kiln stopped the start", exact: true })).toHaveCount(0);
    });
  });
}
