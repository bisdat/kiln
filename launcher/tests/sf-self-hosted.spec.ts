import { expect, test } from "@playwright/test";

for (const deck of [false, true]) {
  test.describe(deck ? "Deck self hosting" : "desktop self hosting", () => {
    test.use({ viewport: deck ? { width: 1280, height: 800 } : { width: 1024, height: 640 } });
    for (const mode of ["before", "started", "removed", "hosted", "empty", "takeover"]) {
      test(`Mods notice: ${mode}`, async ({ page }) => {
        await page.goto(`/?sfSelf=${mode}${deck ? "&deck=steamdeck" : ""}#/${deck ? "deck/mods" : ""}`);
        await page.locator(".k-splash").waitFor({ state: "detached" });
        const notice = page.getByRole("status").filter({ hasText: "ShroudForge runs itself" });
        if (mode === "before" || mode === "started" || mode === "takeover") {
          await expect(notice).toHaveCount(1);
          await expect(notice).toContainText(mode === "takeover" ? "(dinput8.dll)" : "(dbghelp.dll)");
          if (mode === "takeover") {
            const link = notice.getByRole("link", { name: "Let Kiln manage ShroudForge" });
            await expect(link).toHaveAttribute("href", "#/runtime");
            await link.focus();
            await expect(link).toBeFocused();
          }
          await expect(notice).toContainText("Your profile's ShroudForge switches do not apply.");
          await expect(notice).toBeVisible();
          const box = await notice.boundingBox();
          expect(box!.x).toBeGreaterThanOrEqual(0);
          expect(box!.y + box!.height).toBeLessThanOrEqual(deck ? 800 : 640);
        } else {
          await expect(page.getByText("Better Chests", { exact: true }).first()).toBeVisible();
          await expect(notice).toHaveCount(0);
        }
      });
    }
  });
}
for (const mode of ["before", "started", "removed", "hosted"]) {
  test(`Engines hosting row: ${mode}`, async ({ page }) => {
    await page.goto(`/?sfSelf=${mode}#/mods/engines`);
    const row = page.locator('[data-engine="shroudforge"]');
    await expect(row).toBeVisible();
    if (mode === "before" || mode === "started") await expect(row).toContainText("runs itself: dbghelp.dll");
    else if (mode === "removed") {
      await expect(row).toContainText("Last start: self-hosted (loader no longer detected)");
      await expect(row).not.toContainText("runs itself:");
    } else await expect(row).toContainText("hosted");
  });
}
