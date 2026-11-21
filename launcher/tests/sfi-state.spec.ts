// the Engines view in kiln mode in mock mode: the ShroudForge row is kiln-sfi's record (the
// phase word, its reason, one line per package, the start before it apart); a record that is not this start's and a file
// Kiln cannot read show as unknown with why; the restore's BASE is named by its exact label with Steam's Verify for
// vanilla, and unknown when Kiln cannot tell.
import { expect, test, type Page } from "@playwright/test";

async function engines(page: Page, query: string) {
  await page.goto(`/?${query}#/logs`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await page.getByRole("tab", { name: "Engines" }).click();
  return page.locator('[data-engine="shroudforge"]');
}

test("kiln mode: the ShroudForge row says the interpreter's phase, reason and packages", async ({ page }) => {
  const row = await engines(page, "sfiRow=running");
  await expect(row).toHaveAttribute("data-sfi-phase", "running");
  await expect(row).toContainText("Kiln's interpreter (kiln-sfi)");
  await expect(row.locator(".k-tone-ok").first()).toHaveText("running");
  await expect(row.locator('[data-sfi-package="combat_tweaks"]')).toContainText("skipped: it needs runtime.ui.draw_list");
  await expect(row.locator('[data-sfi-package="combat_tweaks"] .k-tone-warn')).toHaveText("skipped");
  await expect(row.locator('[data-sfi-package="sf-unlock-blueprints"]')).toContainText("data applied");
  await expect(row.locator("[data-sfi-previous]")).toContainText("ended without finishing its record");
  const refused = await engines(page, "sfiRow=refused");
  await expect(refused).toHaveAttribute("data-sfi-phase", "refused");
  await expect(refused.locator("[data-sfi-reason]")).toHaveText("kiln-eml's startup pass granted no archive generation");
});

test("kiln mode: a record that is not this start's and a broken file are unknown, never running", async ({ page }) => {
  for (const [q, text] of [["sfiRow=stale", "The record says running, but the game is not running"], ["sfiRow=corrupt", "expected value at line 1"]]) {
    const row = await engines(page, q);
    await expect(row.locator(".k-word-sm")).toHaveText("unknown");
    await expect(row.locator(".k-word-sm")).toHaveClass(/k-tone-warn/);
    await expect(row.locator("[data-sfi-reason]")).toContainText(text);
    await expect(row.locator("[data-sfi-package]")).toHaveCount(0);
  }
});

test("kiln mode: the restore's BASE by its exact label, never vanilla", async ({ page }) => {
  await engines(page, "restoreBase=recorded");
  const base = page.locator('[data-base-origin="first-managed-unclassified"]');
  await expect(base).toContainText("Kiln started from these game files: unclassified: game files as found when Kiln first managed this install.");
  await expect(base).toContainText("Verify integrity of game files in Steam");
  await expect(base).not.toContainText(/vanilla/i);
  await engines(page, "restoreBase=unknown");
  await expect(page.locator('[data-base-origin="unknown"]')).toContainText("Kiln cannot tell which game files it would put back");
  await engines(page, "");
  await expect(page.locator("[data-base-origin]")).toHaveCount(0);
});
