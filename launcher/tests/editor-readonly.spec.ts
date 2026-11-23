// Read-only saves keep navigation available, but no item, character, skill or Knowledge mutation control is active.
import { expect, test, type Locator, type Page } from "@playwright/test";

async function disabledControls(root: Locator) {
  await expect(root.locator("button, input, select").first()).toBeDisabled();
  await expect(root.locator("button:enabled, input:enabled, select:enabled")).toHaveCount(0);
}

async function snapshot(page: Page) {
  return page.evaluate(async () => {
    const load = (path: string) => import(/* @vite-ignore */ path);
    const [{ useSession }, commands] = await Promise.all([load("/src/editor/kiln/session.js"), load("/src/api/commands.ts")]);
    const { model, dirty } = useSession.getState();
    return { model: JSON.stringify(model), dirty, bytes: Array.from(new Uint8Array(await commands.editorReadFile(model.path))) };
  });
}

for (const deck of [false, true]) {
  test(`${deck ? "Deck" : "desktop"} read-only controls are disabled throughout while every view remains browsable`, async ({ page }) => {
    await page.goto(deck ? "/?deck=steamdeck#/deck/editor" : "/#/editor");
    const title = page.locator(deck ? ".dke-name" : ".ke .k-page-title");
    await expect(title).toContainText("Aldric", { timeout: 15000 });
    await page.locator(deck ? '[data-deck-id="ed-characters"]' : ".ke-who-btn").click();
    await page.locator(deck ? '[data-deck-id="char-character2.ksd"]' : '[role="option"]').filter({ hasText: "Mordred" }).click();
    await expect(title).toContainText("Mordred");
    const before = await snapshot(page);
    expect(before.dirty).toBe(false);
    const tab = async (id: string, name: string) => {
      if (deck) await page.locator(`[data-deck-id="ed-tab-${id}"]`).click();
      else await page.getByRole("tab", { name: id === "know" ? /^Knowledge/ : name, exact: true }).click();
    };

    // Known stacks, weapons/gems/durability, water and empty slots all remain inspectable.
    for (const key of ["main:0", "main:24", "main:4", "main:10"]) {
      const slot = page.locator(`[data-deck-id="slot-${key}"]`);
      await expect(slot).toBeEnabled();
      await slot.click();
      await disabledControls(page.locator(".ke-insp"));
    }
    if (!deck) {
      await expect(page.getByRole("button", { name: "Add item", exact: true })).toBeDisabled();
      await expect(page.getByRole("button", { name: "Move", exact: true })).toBeDisabled();
    }

    await tab("char", "Equipment");
    await disabledControls(page.locator(".ke-facts"));
    const worn = page.locator('[data-deck-id="worn-5"]');
    await expect(worn).toBeEnabled();
    await worn.click();
    await disabledControls(page.locator(".ke-insp"));

    await tab("skills", "Skills");
    const nodes = page.locator(deck ? ".ke-tree-key" : '.ke-node[role="button"]');
    expect(await nodes.count()).toBeGreaterThan(0);
    expect(await nodes.evaluateAll((els) => els.every((el) => el.getAttribute("aria-disabled") === "true"))).toBe(true);
    await nodes.first().focus();
    await expect(page.locator(".ke-tree-tip")).toBeVisible();
    await page.getByRole("button", { name: "Zoom in", exact: true }).click();

    await tab("know", "Knowledge");
    await expect(page.getByRole("radio", { name: "Everything", exact: true })).toBeEnabled();
    await page.getByRole("radio", { name: "Everything", exact: true }).click();
    await expect(page.locator('.ke-know-table input[type="checkbox"]').first()).toBeDisabled();
    await expect(page.locator('.ke-know-table input[type="checkbox"]:enabled')).toHaveCount(0);
    await expect(page.locator('[data-deck-id="know-remove"]')).toBeDisabled();
    await page.getByRole("textbox", { name: "Search by name or id", exact: true }).fill("no-match-123456");
    await expect(page.locator(".ke-know-body .ke-know-row")).toHaveCount(0);
    await expect(page.locator(deck ? '[data-deck-id="ed-save"]' : '.ke .k-page-actions button[title="Ctrl+S"]')).toBeDisabled();
    expect(await snapshot(page)).toEqual(before);
  });
}
