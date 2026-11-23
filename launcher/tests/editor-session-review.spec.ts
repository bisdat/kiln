import { expect, test, type Page } from "@playwright/test";

async function open(page: Page, deck = false) {
  await page.goto(deck ? "/?ui=deck#/deck/editor" : "/#/editor");
  await expect(page.locator(deck ? ".dke-name" : ".k-page-title")).toContainText("Aldric", { timeout: 15000 });
  await page.evaluate(async () => {
    const session = "/src/editor/kiln/session.js", platform = "/src/editor/src/platform.js", commands = "/src/api/commands.ts";
    Object.assign(window, { editorSession: await import(session), editorApi: (await import(platform)).api, editorCommands: await import(commands) });
  });
}

test("Deck reselecting the current character supersedes an opening choice", async ({ page }) => {
  await open(page, true);
  await page.evaluate(() => (window as any).__kilnMock.planCommand("editor_read_file", { gate: "older-open" }));
  await page.locator('[data-deck-id="ed-characters"]').click();
  await page.locator('[data-deck-id="char-character1.ksd"]').click();
  await expect(page.getByTestId("deck-editor-say")).toContainText("Opening character");
  await page.locator('[data-deck-id="ed-characters"]').click();
  await page.locator('[data-deck-id="char-character0.ksd"]').click();
  await page.evaluate(() => (window as any).__kilnMock.openGate("older-open"));
  await expect.poll(() => page.evaluate(() => (window as any).editorSession.useSession.getState().opening)).toBe(false);
  await expect(page.locator(".dke-name")).toHaveText("Aldric");
});

test("a superseded same-file read cannot replace the serialization cache", async ({ page }) => {
  await open(page);
  const result = await page.evaluate(async () => {
    const w = window as any, s = w.editorSession, api = w.editorApi, commands = w.editorCommands;
    const path = s.useSession.getState().model.path;
    const oldBytes = Array.from(new Uint8Array(await commands.editorReadFile(path)));
    // Hold an older snapshot before the real parser sees it. A subsequent read sees the game's newer bytes.
    w.__kilnMock.planCommand("editor_read_file", { gate: "old-snapshot", result: oldBytes });
    const earlier = s.openSave(path);
    await w.__kilnMockEditor.gameWrote("character0.ksd", "character1.ksd");
    await s.openSave(path);
    const chosen = structuredClone(s.useSession.getState().model);
    const parser = "/src/editor/shared/sv2c-web.mjs";
    const sv2c = await import(parser);
    const decompress = async () => { throw new Error("Unexpected compressed mock block"); };
    const before = await sv2c.parse(new Uint8Array(await commands.editorReadFile(path)), decompress, []);
    w.__kilnMock.openGate("old-snapshot");
    await earlier;
    // Editing inventory must not revert unrelated character_data to the older snapshot's name.
    chosen.main[0].count = 250;
    s.setModel(chosen);
    const saved = await s.saveModel();
    const bytes = new Uint8Array(await commands.editorReadFile(path));
    const container = await sv2c.parse(bytes, decompress, ["character_data"]);
    const unchangedBlocks = container.blocks.filter((b: { name: string }) => b.name !== "player_character").every((b: { name: string; raw: Uint8Array }) =>
      JSON.stringify(Array.from(b.raw)) === JSON.stringify(Array.from(before.blocks.find((old: { name: string }) => old.name === b.name).raw)));
    const disk = await api.openSave(path);
    return { chosenName: chosen.name, saved, serializedName: sv2c.characterName(container), serializedCount: disk.main[0].count, unchangedBlocks };
  });
  expect(result).toEqual({ chosenName: "Brienne", saved: "saved", serializedName: "Brienne", serializedCount: 250, unchangedBlocks: true });
});

test("a catalog opened over Save keeps its selection until adding is safe", async ({ page }) => {
  await open(page);
  await page.locator(".ke-grid .ke-slot.full").first().click();
  await page.locator('.ke-insp [aria-label="+1"]').first().click();
  await page.getByRole("button", { name: "Add item", exact: true }).click();
  const dialog = page.locator('.k-modal:has(.ke-cat)');
  await dialog.getByRole("textbox").fill("iron bars");
  await dialog.locator('.ke-cat-grid [aria-label="Iron Bars"]').click();
  await page.evaluate(() => (window as any).__kilnMock.planCommand("editor_write_save", { gate: "catalog-save" }));
  await page.keyboard.press("Control+s");
  await expect.poll(() => page.evaluate(() => (window as any).editorSession.useSession.getState().saving)).toBe(true);
  const add = dialog.getByRole("button", { name: "Add", exact: true });
  await expect(dialog).toHaveAttribute("inert", "");
  if (process.env.KILN_DECK_SHOTS) await page.screenshot({ path: `${process.env.KILN_DECK_SHOTS}/session-catalog-saving.png` });
  // A programmatically dispatched event must not reach the immediate-add path either.
  await dialog.locator('.ke-cat-grid [aria-label="Iron Bars"]').dispatchEvent("dblclick");
  await expect(dialog).toBeVisible();
  await page.evaluate(() => (window as any).__kilnMock.openGate("catalog-save"));
  await expect(dialog).not.toHaveAttribute("inert", "");
  await expect(dialog.locator('.ke-cat-grid [aria-label="Iron Bars"]')).toBeFocused();
  await expect(add).toBeEnabled();
  await add.click();
  await expect(dialog).toHaveCount(0);
  expect(await page.evaluate(() => {
    const state = (window as any).editorSession.useSession.getState();
    return { count: state.model.main.find((entry: { slot: number }) => entry.slot === 10)?.count, dirty: state.dirty };
  })).toEqual({ count: 250, dirty: true });
  await page.evaluate(() => (window as any).__kilnMock.planCommand("editor_write_save", { gate: "shortcut-save" }));
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await page.keyboard.press("i");
  await expect(dialog).toHaveCount(0);
  await page.keyboard.press("Delete");
  await page.evaluate(() => (window as any).__kilnMock.openGate("shortcut-save"));
  await expect.poll(() => page.evaluate(() => (window as any).editorSession.useSession.getState().saving)).toBe(false);
  expect(await page.evaluate(async () => {
    const w = window as any, model = w.editorSession.useSession.getState().model;
    const disk = await w.editorApi.openSave(model.path);
    return disk.main.find((entry: { slot: number }) => entry.slot === 10)?.count;
  })).toBe(250);
});

test("Ctrl+S uses the last value committed before Save starts, even before the view rerenders", async ({ page }) => {
  await open(page);
  const result = await page.evaluate(async () => {
    const w = window as any, s = w.editorSession;
    const m = structuredClone(s.useSession.getState().model);
    m.main[0].count = 250;
    s.setModel(m);
    // Immediate commits from numeric inputs and the shortcut can run before React paints the new model.
    window.dispatchEvent(new KeyboardEvent("keydown", { key: "s", ctrlKey: true, cancelable: true }));
    const captured = s.useSession.getState().saving;
    while (s.useSession.getState().saving) await new Promise((resolve) => setTimeout(resolve, 10));
    const disk = await w.editorApi.openSave(m.path);
    return { captured, count: disk.main[0].count, dirty: s.useSession.getState().dirty };
  });
  expect(result).toEqual({ captured: true, count: 250, dirty: false });
});
