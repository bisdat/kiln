import { test, expect } from "@playwright/test";
for (const deck of [false, true]) {
  test.describe(deck ? "Deck Shroudlink addon" : "desktop Shroudlink addon", () => {
    test.use({ viewport: deck ? { width: 1280, height: 800 } : { width: 1024, height: 640 } });
    for (const mode of ["available", "missing", "server", "empty", "off"]) {
      test(`MODS tab notice: ${mode}`, async ({ page }) => {
        await page.goto(`/?slNotice=${mode}${deck ? "&deck=steamdeck" : ""}#/${deck ? "deck/mods" : ""}`);
        await page.locator(".k-splash").waitFor({ state: "detached" });
        const notice = page.getByRole("status").filter({ hasText: "Shroudlink client addon" });
        if (mode === "available" || mode === "missing") {
          await expect(notice).toBeVisible();
          await expect(notice).toContainText("Your Shroudlink mods still run");
          if (mode === "available") {
            await notice.getByRole("button", { name: "Switch addon on" }).click();
            await expect(notice).toHaveCount(0);
            if (deck) await expect(page.locator('[data-deck-id="mod-native-shroudlink-addon"]')).toHaveAttribute("aria-label", "Shroudlink client addon: on");
            else await expect(page.getByRole("switch", { name: "Shroudlink client addon", exact: true })).toBeChecked();
          } else {
            await expect(notice).toContainText("Shroudlink download");
            await expect(notice.getByRole("button")).toHaveCount(0);
          }
        } else {
          if (mode !== "server") await expect(page.getByText("Better Chests", { exact: true }).first()).toBeVisible();
          else await expect(page.getByText(deck ? "No game yet." : "No client found", { exact: true })).toBeVisible();
          await expect(notice).toHaveCount(0);
        }
      });
    }
  });
}
