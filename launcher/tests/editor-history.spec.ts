import { expect, test, type Page } from "@playwright/test";

async function open(page: Page, deck = false) {
  await page.goto(deck ? "/?ui=deck#/deck/editor" : "/#/editor");
  await expect(page.locator(deck ? ".dke-name" : ".k-page-title")).toContainText("Aldric", { timeout: 15000 });
  await page.evaluate(async () => {
    const session = "/src/editor/kiln/session.js", platform = "/src/editor/src/platform.js", commands = "/src/api/commands.ts";
    Object.assign(window, { editorSession: await import(session), editorApi: (await import(platform)).api, editorCommands: await import(commands) });
  });
}
const session = <T,>(page: Page, fn: () => T) => page.evaluate(fn);
async function edit(page: Page) {
  await page.locator(".ke-grid .ke-slot.full").first().click();
  await page.locator('.ke-insp [aria-label="+1"]').first().click();
}
const count = (page: Page) => session(page, () => (window as any).editorSession.useSession.getState().model.main[0].count);

for (const deck of [false, true]) test(`${deck ? "Deck" : "desktop"} undo/redo controls restore edits and respect field undo`, async ({ page }) => {
  await open(page, deck);
  const undo = page.locator(deck ? '[data-deck-id="ed-undo"]' : '[data-testid="editor-undo"]');
  const redo = page.locator(deck ? '[data-deck-id="ed-redo"]' : '[data-testid="editor-redo"]');
  await expect(undo).toBeDisabled(); await expect(redo).toBeDisabled();
  await edit(page);
  await expect(undo).toBeEnabled();
  if (!deck) await expect(undo).toHaveAttribute("title", /Undo: Change amount/);
  await undo.click(); expect(await count(page)).toBe(120);
  await redo.click(); expect(await count(page)).toBe(121);
  // Focus outside editable text. Both shortcut spellings work on each shell.
  await undo.focus(); await page.keyboard.press("Control+z"); expect(await count(page)).toBe(120);
  await page.keyboard.press("Control+Shift+z"); expect(await count(page)).toBe(121);
  await page.keyboard.press("Control+z"); await page.keyboard.press("Control+y"); expect(await count(page)).toBe(121);
  if (deck) {
    await expect(page.locator('.deck-hints [data-hint="ls"]')).toContainText("Undo");
    await expect(page.locator('.deck-hints [data-hint="rs"]')).toContainText("Redo");
    await page.locator('.deck-hints [data-hint="ls"]').click(); expect(await count(page)).toBe(120);
    await page.locator('.deck-hints [data-hint="rs"]').click(); expect(await count(page)).toBe(121);
  } else {
    const field = page.locator('.ke-insp input[aria-label="Amount"]');
    await field.focus(); await page.keyboard.press("Control+z");
    expect(await count(page)).toBe(121); // no editor-wide undo while an input owns focus
    // Paused typing is still one gesture; moving focus to another control ends it.
    await field.fill("125"); await page.waitForTimeout(800); await field.fill("126");
    await undo.click(); expect(await count(page)).toBe(121);
    await redo.click(); expect(await count(page)).toBe(126);
  }
});

test("Deck history controls stay contained while focus and dirty state change", async ({ page }) => {
  await open(page, true); await edit(page);
  const undo = page.locator('[data-deck-id="ed-undo"]'), redo = page.locator('[data-deck-id="ed-redo"]');
  const contained = async () => {
    const controls = page.locator('[data-deck-id="ed-undo"], [data-deck-id="ed-redo"], .deck-hints [data-hint="ls"], .deck-hints [data-hint="rs"]');
    await expect(controls).toHaveCount(4);
    for (const control of await controls.all()) {
      const box = (await control.boundingBox())!;
      expect(box.x).toBeGreaterThanOrEqual(0);
      expect(box.x + box.width).toBeLessThanOrEqual(page.viewportSize()!.width);
    }
  };
  await contained();
  await undo.focus(); await contained();
  await undo.click(); expect(await count(page)).toBe(120);
  await redo.focus(); await contained();
  await redo.click(); expect(await count(page)).toBe(121);
});

test("edit then undo serializes original bytes; saved checkpoints track undo/redo and discard", async ({ page }) => {
  await open(page);
  const result = await session(page, async () => {
    const w = window as any, s = w.editorSession;
    const original = s.useSession.getState().model, path = original.path;
    const bytes = async () => Array.from(new Uint8Array(await w.editorCommands.editorReadFile(path)));
    const before = await bytes();
    s.setModel({ ...original, level: 26 }); s.undo();
    const clean = !s.useSession.getState().dirty;
    // Save is intentionally disabled when clean. Exercise the real serializer with the exact undone model.
    await w.editorApi.writeSave(path, s.useSession.getState().model);
    const same = JSON.stringify(before) === JSON.stringify(await bytes());
    await s.openSave(path);
    s.setModel({ ...s.useSession.getState().model, level: 26 }); await s.saveModel();
    const savedCleared = !s.canUndo(s.useSession.getState()) && !s.canRedo(s.useSession.getState());
    s.setModel({ ...s.useSession.getState().model, xp: 1300 }); s.discard();
    const discardedClean = !s.useSession.getState().dirty;
    s.undo(); const recovered = s.useSession.getState().model.xp;
    s.undo(); const undoClean = !s.useSession.getState().dirty;
    s.redo(); const redoDirty = s.useSession.getState().dirty;
    await s.saveModel();
    const afterSaveCleared = !s.undo() && !s.redo();
    return { clean, same, savedCleared, discardedClean, recovered, undoClean, redoDirty, afterSaveCleared };
  });
  expect(result).toEqual({ clean: true, same: true, savedCleared: true, discardedClean: true, recovered: 1300, undoClean: true, redoDirty: true, afterSaveCleared: true });
});

test("saving equipment removal adopts dense normalized slots and explicitly resets history", async ({ page }) => {
  await open(page);
  expect(await session(page, async () => {
    const w = window as any, s = w.editorSession, original = s.useSession.getState().model, path = original.path;
    s.setModel({ ...original, equipment: original.equipment.slice(1) });
    const removed = await s.saveModel();
    const normalized = s.useSession.getState().model;
    const disk = await w.editorApi.openSave(path);
    return { removed, undo: s.undo(), redo: s.redo(), slots: normalized.equipment.map((e: any) => e.slot),
      same: JSON.stringify(normalized.equipment) === JSON.stringify(disk.equipment),
      maskMatches: normalized.equipMask === disk.equipMask };
  })).toEqual({ removed: "saved", undo: false, redo: false, slots: [0, 1, 2, 3, 4, 5, 6, 7], same: true, maskMatches: true });
  await expect(page.getByTestId("editor-undo")).toBeDisabled();
  await expect(page.getByTestId("editor-redo")).toBeDisabled();
  await expect(page.getByTestId("editor-undo")).toHaveAttribute("title", /Nothing to undo since the last save/);
});

test("busy save/open/restore cannot consume history; successful reads reset it", async ({ page }) => {
  await open(page); await edit(page);
  for (const [command, operation] of [["editor_write_save", "save"], ["editor_read_file", "open"], ["editor_restore_backup", "restore"]]) {
    await session(page, async () => {
      const w = window as any, s = w.editorSession, m = s.useSession.getState().model;
      s.setModel({ ...m, xp: m.xp + 1 });
    });
    await page.evaluate(async ([command, operation]) => {
      const w = window as any, s = w.editorSession, path = s.useSession.getState().model.path;
      const backups = operation === "restore" ? await w.editorApi.listBackups(path) : [];
      w.__kilnMock.planCommand(command, { gate: "history-busy" });
      w.historyOperation = operation === "save" ? s.saveModel() : operation === "open" ? s.openSave(path) : s.restoreBackup(backups[0]);
    }, [command, operation]);
    await expect(page.getByRole("button", { name: "Undo", exact: true })).toBeDisabled();
    await expect(page.getByRole("button", { name: "Redo", exact: true })).toBeDisabled();
    expect(await session(page, () => {
      const s = (window as any).editorSession, h = s.useSession.getState().history;
      const undo = s.undo(), redo = s.redo();
      return { undo, redo, untouched: s.useSession.getState().history === h };
    })).toEqual({ undo: false, redo: false, untouched: true });
    await session(page, async () => { const w = window as any; w.__kilnMock.openGate("history-busy"); await w.historyOperation; });
    if (operation !== "save") await expect(page.getByRole("button", { name: "Undo", exact: true })).toBeDisabled();
  }
});

test("Read again and another character reset history only after the existing discard confirmation", async ({ page }) => {
  await open(page); await edit(page);
  await session(page, () => (window as any).editorSession.setStatus({ tone: "err", again: true, text: "Changed on disk" }));
  await page.getByRole("button", { name: "Read again" }).click();
  const dialog = page.getByRole("dialog", { name: "Discard the changes?" });
  await dialog.getByRole("button", { name: "Cancel" }).click();
  await expect(page.getByTestId("editor-undo")).toBeEnabled();
  await page.getByRole("button", { name: "Read again" }).click();
  await dialog.getByRole("button", { name: "Discard and open" }).click();
  await expect(page.getByTestId("editor-undo")).toBeDisabled();
  await expect(page.getByTestId("editor-redo")).toBeDisabled();
  await edit(page);
  await page.locator(".ke-who-btn").click();
  await page.getByRole("option", { name: /Brienne/ }).click();
  await expect(dialog).toBeVisible();
  await dialog.getByRole("button", { name: "Discard and open" }).click();
  await expect(page.locator(".k-page-title")).toContainText("Brienne");
  await expect(page.getByTestId("editor-undo")).toBeDisabled();
  await expect(page.getByTestId("editor-redo")).toBeDisabled();
});

for (const deck of [false, true]) test(`${deck ? "Deck" : "desktop"} history buttons fit the Russian minimum viewport`, async ({ browser }) => {
  const viewport = deck ? { width: 1280, height: 800 } : { width: 1024, height: 640 };
  const context = await browser.newContext({ locale: "ru", viewport });
  const page = await context.newPage();
  await page.addInitScript(() => Object.defineProperty(navigator, "languages", { get: () => ["ru"] }));
  await page.goto(deck ? "/?ui=deck#/deck/editor" : "/#/editor");
  await expect(page.locator(deck ? '.dke-name' : '.k-page-title')).toContainText('Aldric');
  await edit(page); // The dirty toolbar also shows Discard: test its widest state.
  const controls = page.locator(deck ? '[data-deck-id="ed-undo"], [data-deck-id="ed-redo"], .deck-hints [data-hint="ls"], .deck-hints [data-hint="rs"]' : '[data-testid="editor-undo"], [data-testid="editor-redo"]');
  await expect(controls).toHaveCount(deck ? 4 : 2);
  await expect(controls.first()).toBeVisible();
  for (const control of await controls.all()) {
    const box = (await control.boundingBox())!;
    expect(box.x).toBeGreaterThanOrEqual(0); expect(box.x + box.width).toBeLessThanOrEqual(viewport.width);
    expect(box.y).toBeGreaterThanOrEqual(0); expect(box.y + box.height).toBeLessThanOrEqual(viewport.height);
  }
  await context.close();
});
