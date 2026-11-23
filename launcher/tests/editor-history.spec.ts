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

for (const locale of ["es", "ru"]) test(`Deck history leaves the entire hint row and inspector contained in ${locale}`, async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.route("**/assets-local/editor/**", route => route.abort());
  await open(page, true);
  await page.evaluate(async (locale) => {
    const path = "/src/i18n/index.ts";
    (await import(path)).setLanguage(locale);
    const s = (window as any).editorSession, model = s.useSession.getState().model;
    s.setModel({ ...model, name: "Хранитель пламени с невероятно длинным именем", xp: model.xp + 1 });
    await document.fonts.ready;
    await Promise.all(document.getAnimations().filter(a => a.effect?.getTiming().iterations !== Infinity).map(a => a.finished.catch(() => {})));
  }, locale);
  await page.locator('.ke-grid [data-deck-id="slot-main:0"]').click();
  // Sample the related boxes in one browser turn: focusing the slot updates the controller's action labels.
  const geometry = await page.evaluate(() => {
    const box = (selector: string) => document.querySelector(selector)!.getBoundingClientRect().toJSON();
    return { split: box(".ke-split"), inspector: box(".ke-insp"), footer: box(".deck-hints"),
      hints: [...document.querySelectorAll(".deck-hint")].map(el => el.getBoundingClientRect().toJSON()) };
  });
  expect(geometry.inspector.bottom, "inspector bottom").toBeLessThanOrEqual(geometry.split.bottom + 1);
  for (const hint of geometry.hints) {
    expect.soft(hint.left, "hint left").toBeGreaterThanOrEqual(geometry.footer.left);
    expect.soft(hint.right, "hint right").toBeLessThanOrEqual(geometry.footer.right);
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

test("reading a save is not writing one: the shell's busy flag (close guard, Quit, update restart) holds only for save and restore", async ({ page }) => {
  await open(page);
  for (const [command, operation, busy] of [["editor_read_file", "open", false], ["editor_write_save", "save", true], ["editor_restore_backup", "restore", true]] as const) {
    if (operation === "save") await edit(page);
    await page.evaluate(async ([command, operation]) => {
      const w = window as any, s = w.editorSession, path = s.useSession.getState().model.path;
      const backups = operation === "restore" ? await w.editorApi.listBackups(path) : [];
      w.__kilnMock.planCommand(command, { gate: "busy-flag" });
      w.busyOperation = operation === "save" ? s.saveModel() : operation === "open" ? s.openSave(path) : s.restoreBackup(backups[0]);
    }, [command, operation]);
    expect(await session(page, async () => {
      const s = (window as any).editorSession.useSession.getState();
      const pending = (await import("/src/editor/kiln/pending.ts")).usePendingEdits.getState();
      return { blocked: !!(s.saving || s.restoring || s.opening), busy: pending.busy };
    }), operation).toEqual({ blocked: true, busy });
    await session(page, async () => { const w = window as any; w.__kilnMock.openGate("busy-flag"); await w.busyOperation; });
    expect(await session(page, async () => (await import("/src/editor/kiln/pending.ts")).usePendingEdits.getState().busy), operation).toBe(false);
  }
});

test("Deck Read again opens the discard confirmation without moving its pointer target", async ({ page }) => {
  await open(page, true);
  await session(page, () => {
    const s = (window as any).editorSession, m = structuredClone(s.useSession.getState().model);
    m.main[0].count = 121; s.setModel(m); s.setStatus({ tone: 'err', again: true, text: 'Changed on disk' });
  });
  expect(await session(page, () => (window as any).editorSession.useSession.getState().dirty)).toBe(true);
  await page.locator('[data-deck-id="ed-reread"]').click();
  const dialog = page.getByRole('dialog', { name: 'Discard the changes?' });
  await expect(dialog).toBeVisible();
  await dialog.getByRole('button', { name: /Cancel/ }).click();
  expect(await count(page)).toBe(121);
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

const draftHistoryState = (page: Page) => page.evaluate(() => {
  const s = (window as any).editorSession.useSession.getState();
  return { count: s.model.main[0].count, dirty: s.dirty, past: s.history.past.length, future: s.history.future.length };
});
const pressHistoryStick = async (page: Page, button: string) => {
  for (const pressed of [true, false]) await page.evaluate(([button, pressed]) => (window as any).__kilnMock.emit("kiln://gamepad", { kind: "button", button, pressed, repeat: false, device: "event9" }), [button, pressed]);
};

test("empty numeric drafts keep native undo and do not swallow editor history on blur", async ({ page }) => {
  await open(page);
  await page.locator('.ke-grid [data-deck-id="slot-main:0"]').click();
  await page.locator('.ke-insp [aria-label="+1"]').first().click();
  const field = page.locator('.ke-insp input[aria-label="Amount"]');
  await field.fill("");
  expect(await draftHistoryState(page)).toEqual({ count: 121, dirty: true, past: 1, future: 0 });
  // Synthetic shortcut events test routing without depending on Chromium's separate native text undo stack.
  for (const [key, shift] of [["z", false], ["y", false], ["z", true]] as const) {
    expect(await field.evaluate((element, [key, shift]) => element.dispatchEvent(new KeyboardEvent("keydown", { key, shiftKey: shift, ctrlKey: true, bubbles: true, cancelable: true })), [key, shift] as const)).toBe(true);
    expect(await draftHistoryState(page)).toEqual({ count: 121, dirty: true, past: 1, future: 0 });
  }
  await page.getByTestId("editor-undo").click();
  await expect(field).toHaveValue("120");
  expect(await draftHistoryState(page)).toEqual({ count: 120, dirty: false, past: 0, future: 1 });
  await page.getByTestId("editor-redo").click();
  await expect(field).toHaveValue("121");
});

test("a typed numeric gesture and its explicit draft commit make exactly one history entry", async ({ page }) => {
  await open(page);
  await page.locator('.ke-grid [data-deck-id="slot-main:0"]').click();
  const field = page.locator('.ke-insp input[aria-label="Amount"]');
  await field.fill("12"); await field.fill("125"); await field.press("Enter");
  expect(await draftHistoryState(page)).toEqual({ count: 125, dirty: true, past: 1, future: 0 });
  await page.getByTestId("editor-undo").click();
  await expect(field).toHaveValue("120");
  expect(await draftHistoryState(page)).toEqual({ count: 120, dirty: false, past: 0, future: 1 });
});

test("Deck stick presses undo and redo; an open number-pad draft cannot consume history", async ({ page }) => {
  await open(page, true);
  await page.locator('.ke-grid [data-deck-id="slot-main:0"]').click();
  await page.locator('.ke-insp [aria-label="+1"]').first().click();
  await pressHistoryStick(page, "ls");
  await expect.poll(() => draftHistoryState(page)).toEqual({ count: 120, dirty: false, past: 0, future: 1 });
  await pressHistoryStick(page, "rs");
  await expect.poll(() => draftHistoryState(page)).toEqual({ count: 121, dirty: true, past: 1, future: 0 });
  await page.locator('[data-deck-id="value-Amount"]').click();
  const dialog = page.locator('[data-deck-dialog="editor-number"]');
  await expect(dialog).toBeVisible();
  await page.locator('[data-deck-id="pad-5"]').click();
  await pressHistoryStick(page, "ls"); await pressHistoryStick(page, "rs");
  expect(await draftHistoryState(page)).toEqual({ count: 121, dirty: true, past: 1, future: 0 });
  await dialog.getByRole("button", { name: "Cancel", exact: true }).click();
  await pressHistoryStick(page, "ls");
  await expect.poll(() => draftHistoryState(page)).toEqual({ count: 120, dirty: false, past: 0, future: 1 });
});
