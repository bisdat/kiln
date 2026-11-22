import { expect, test } from "@playwright/test";

for (const { surface, url } of [
  { surface: "desktop Mods", url: "/?emlR=present#/" },
  { surface: "Deck Mods", url: "/?emlR=present&deck=steamdeck#/deck/mods" },
  { surface: "Engines", url: "/?emlR=present#/mods/engines" },
]) {
  test(`EML-R is identified on ${surface}`, async ({ page }) => {
    await page.goto(url);
    await page.locator(".k-splash").waitFor({ state: "detached" });
    const notice = page.getByRole("status").filter({ hasText: "Kiln found EML-R's loader (dbghelp.dll)" });
    await expect(notice).toBeVisible();
    await expect(notice).toContainText("loads the mods on its own list when it starts");
    await expect(notice).toContainText("independently of Kiln and its profile switches");
    await expect(notice).toContainText("Kiln's ShroudForge interpreter stands aside");
    await expect(page.getByText("ShroudForge's own loader is installed", { exact: false })).toHaveCount(0);
    if (surface === "Engines") {
      const shroudforge = page.locator('[data-engine="shroudforge"]');
      await expect(shroudforge).toBeVisible();
      await expect(shroudforge).not.toContainText("own loader installed: dbghelp.dll");
    }
  });
}
