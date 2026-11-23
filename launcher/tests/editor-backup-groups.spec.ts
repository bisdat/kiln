import { expect, test } from "@playwright/test";

for (const deck of [false, true]) {
  test(`${deck ? "Deck" : "desktop"} separates backups with an unknown origin, keeps old responses matching and allows legacy restore`, async ({ page }) => {
    await page.goto(deck ? "/?deck=steamdeck#/deck/editor" : "/#/editor");
    await expect(page.locator(deck ? ".dke-name" : ".k-page-title")).toContainText("Aldric", { timeout: 15000 });
    await page.evaluate(async () => {
      const module = "/src/editor/src/platform.js";
      const api = (await import(module)).api;
      const current = (await api.listSaves())[0];
      const backups = await api.listBackups(current.path);
      // Responses from older backends have no group: they still belong in the matching section. Unknown-origin
      // recovery choices are deliberately listed first by the backend here: the UI must move them below matching.
      const list = api.listBackups;
      api.listBackups = async () => [{ ...backups[1], group: "legacy" }, backups[0]];
      Object.assign(window, { originalBackupList: list });
    });
    await page.locator(deck ? '[data-deck-id="ed-backups"]' : ".k-page-actions button").filter({ hasText: /^Backups$/ }).click();
    const dialog = page.getByRole("dialog", { name: "Backups for Aldric" });
    const heading = dialog.getByRole("heading", { name: "Older backups (origin unknown)" });
    await expect(heading).toBeVisible();
    await expect(dialog).toContainText("These backups may belong to another save. Check the character after restoring.");
    const row = deck ? dialog.locator(".deck-row") : dialog.locator(".ke-bk-row");
    await expect(row).toHaveCount(2);
    expect(await row.first().evaluate((el) => el.compareDocumentPosition(document.querySelector('[data-backup-group="legacy"]')!) & Node.DOCUMENT_POSITION_FOLLOWING)).toBeTruthy();
    if (!deck) await expect(row.first()).toContainText("backup-1");
    if (deck) await row.last().click();
    else await row.last().getByRole("button", { name: "Restore", exact: true }).click();
    if (deck) await page.locator('[data-deck-id="restore-confirm"]').click();
    else await row.last().getByRole("button", { name: "Restore", exact: true }).click();
    await expect(page.getByRole("dialog")).toHaveCount(0);
    await expect(page.locator(deck ? ".dke-name" : ".k-page-title")).toContainText("Aldric");
  });
}
