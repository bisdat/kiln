import { expect, test, type Page } from "@playwright/test";

async function open(page: Page) {
  await page.goto("/#/editor");
  await expect(page.locator(".k-page-title")).toContainText("Aldric", { timeout: 15000 });
  await page.evaluate(async () => {
    const session = "/src/editor/kiln/session.js", platform = "/src/editor/src/platform.js", commands = "/src/api/commands.ts";
    Object.assign(window, { editorSession: await import(session), editorApi: (await import(platform)).api, editorCommands: await import(commands) });
  });
}

test("destructive undo and redo reproduce exact original and edited SV2C bytes", async ({ page }) => {
  await open(page);
  const outcomes = await page.evaluate(async () => {
    const w = window as any, s = w.editorSession, api = w.editorApi, commands = w.editorCommands;
    const path = s.useSession.getState().model.path;
    const readBytes = async () => Array.from(new Uint8Array(await commands.editorReadFile(path)));
    const originalBytes = await readBytes();
    // Exercise the real writer, then restore its private parser baseline as well as the synthetic file. This is not
    // a successful session Save: snapshots must be compared against the same original container and equipment mask.
    const serialize = async (model: any) => {
      await api.writeSave(path, model);
      const result = await readBytes();
      await commands.editorWriteSave(path, originalBytes);
      await api.openSave(path);
      return result;
    };
    const edits = [
      (m: any) => { m.main[0].count = 125; },
      (m: any) => { m.main.splice(0, 2); },
      (m: any) => { m.extra.splice(0, 1); },
      (m: any) => { const a = m.main[0].slot; m.main[0].slot = m.main[1].slot; m.main[1].slot = a; },
      (m: any) => { m.equipment.splice(0, 2); },
      (m: any) => { m.skills[0].level = 0; },
      (m: any) => { m.knowledge.splice(0, 25); },
    ];
    const outcomes = [];
    for (const edit of edits) {
      await s.openSave(path);
      const model = structuredClone(s.useSession.getState().model); edit(model); s.setModel(model);
      const editedBytes = await serialize(s.useSession.getState().model);
      s.undo(); const clean = !s.useSession.getState().dirty;
      const undoneBytes = await serialize(s.useSession.getState().model);
      s.redo(); const dirty = s.useSession.getState().dirty;
      const redoneBytes = await serialize(s.useSession.getState().model);
      outcomes.push({ clean, dirty, changed: JSON.stringify(editedBytes) !== JSON.stringify(originalBytes),
        original: JSON.stringify(undoneBytes) === JSON.stringify(originalBytes),
        edited: JSON.stringify(redoneBytes) === JSON.stringify(editedBytes), magic: undoneBytes.slice(0, 4) });
    }
    return outcomes;
  });
  expect(outcomes).toHaveLength(7);
  for (const outcome of outcomes) expect(outcome).toEqual({ clean: true, dirty: true, changed: true, original: true, edited: true, magic: [83, 86, 50, 67] });
});

test("restore invalidates old history even when the subsequent read fails", async ({ page }) => {
  await open(page);
  const result = await page.evaluate(async () => {
    const w = window as any, s = w.editorSession, api = w.editorApi;
    const model = s.useSession.getState().model;
    s.setModel({ ...model, xp: model.xp + 1 }); s.undo(); // both an old checkpoint and a redo candidate
    const backup = (await api.listBackups(model.path))[0];
    w.__kilnMock.planCommand("editor_read_file", { error: { code: "io", message: "Read failed after restore" } });
    const restored = await s.restoreBackup(backup);
    const state = s.useSession.getState();
    return { restored, undo: s.undo(), redo: s.redo(), dirty: state.dirty, model: state.model?.name ?? null, status: state.status?.text };
  });
  expect(result).toEqual({ restored: false, undo: false, redo: false, dirty: false, model: null, status: "Read failed after restore" });
});

test("a failed restore preserves the current edit and its history", async ({ page }) => {
  await open(page);
  const result = await page.evaluate(async () => {
    const w = window as any, s = w.editorSession, model = s.useSession.getState().model;
    s.setModel({ ...model, xp: 1300 });
    const history = s.useSession.getState().history;
    const backup = (await w.editorApi.listBackups(model.path))[0];
    w.__kilnMock.planCommand("editor_restore_backup", { error: { code: "io", message: "Restore failed" } });
    let error = "";
    try { await s.restoreBackup(backup); } catch (e) { error = (e as Error).message; }
    const state = s.useSession.getState();
    return { error, same: state.history === history, xp: state.model.xp, dirty: state.dirty, undo: s.undo(), originalXp: s.useSession.getState().model.xp };
  });
  expect(result).toEqual({ error: "Restore failed", same: true, xp: 1300, dirty: true, undo: true, originalXp: 1200 });
});

test("bounded repeated history travel and a new edit cannot replay a different character", async ({ page }) => {
  await open(page);
  const result = await page.evaluate(async () => {
    const s = (window as any).editorSession, model = s.useSession.getState().model;
    // Distinct scalar fields force boundaries without relying on wall-clock delays.
    for (let i = 0; i < 130; i++) {
      const m = s.useSession.getState().model;
      s.setModel(i % 2 ? { ...m, xp: m.xp + 1 } : { ...m, level: m.level + 1 });
    }
    const entries = s.useSession.getState().history.past.length;
    const traveled = Array.from({ length: 140 }, () => s.undo()).filter(Boolean).length;
    const oldest = s.useSession.getState().model;
    s.redo(); s.setModel({ ...s.useSession.getState().model, xp: 9999 });
    const redoCleared = !s.redo();
    await s.openSave(model.path.replace("character0", "character1"));
    return { entries, traveled, oldestLevel: oldest.level, oldestXp: oldest.xp, redoCleared,
      name: s.useSession.getState().model.name, undo: s.undo(), redo: s.redo(), dirty: s.useSession.getState().dirty };
  });
  expect(result).toEqual({ entries: 100, traveled: 100, oldestLevel: 40, oldestXp: 1215, redoCleared: true, name: "Brienne", undo: false, redo: false, dirty: false });
});

test("read-only characters reject both history directions and preserve their bytes", async ({ page }) => {
  await open(page);
  const result = await page.evaluate(async () => {
    const w = window as any, s = w.editorSession;
    const path = s.useSession.getState().model.path.replace("character0", "character2");
    await s.openSave(path);
    const before = Array.from(new Uint8Array(await w.editorCommands.editorReadFile(path)));
    const state = s.useSession.getState();
    const changed = s.setModel({ ...state.model, xp: 9999 });
    return { changed, undo: s.undo(), redo: s.redo(), canUndo: s.canUndo(state), canRedo: s.canRedo(state),
      same: JSON.stringify(before) === JSON.stringify(Array.from(new Uint8Array(await w.editorCommands.editorReadFile(path)))) };
  });
  expect(result).toEqual({ changed: false, undo: false, redo: false, canUndo: false, canRedo: false, same: true });
  await expect(page.getByTestId("editor-undo")).toBeDisabled();
  await expect(page.getByTestId("editor-redo")).toBeDisabled();
});
