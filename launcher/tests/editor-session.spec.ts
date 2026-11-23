// Session regressions use the mounted editor and its real parser/writer over synthetic SV2C bytes. IPC gates make
// the order deterministic; no real character files or native window controls are involved.
import { expect, test, type Page } from "@playwright/test";

const SHOTS = process.env.KILN_DECK_SHOTS;
async function open(page: Page, deck = false) {
  await page.goto(deck ? "/?ui=deck#/deck/editor" : "/#/editor");
  await expect(page.locator(deck ? ".dke-name" : ".k-page-title")).toContainText("Aldric", { timeout: 15000 });
  await page.evaluate(async () => {
    const session = "/src/editor/kiln/session.js", platform = "/src/editor/src/platform.js", commands = "/src/api/commands.ts";
    Object.assign(window, { editorSession: await import(session), editorApi: (await import(platform)).api, editorCommands: await import(commands) });
  });
}
// The imports above run in Vite, not in the Playwright process.
const run = <T,>(page: Page, fn: () => T) => page.evaluate(fn);
const edit = async (page: Page) => {
  await page.locator(".ke-grid .ke-slot.full").first().click();
  await page.locator('.ke-insp [aria-label="+1"]').first().click();
};
const gate = (page: Page, cmd: string, name: string) => page.evaluate(([cmd, gate]) => {
  (window as any).__kilnMock.planCommand(cmd, { gate });
}, [cmd, name]);
const release = (page: Page, name: string) => page.evaluate((name) => (window as any).__kilnMock.openGate(name), name);

test("a pending restore excludes another restore, captures its target and preserves the chosen serialized bytes", async ({ page }) => {
  await open(page);
  const result = await run(page, async () => {
    const w = window as any, s = w.editorSession, api = w.editorApi, commands = w.editorCommands;
    const path = s.useSession.getState().model.path;
    const change = (count: number) => {
      const m = structuredClone(s.useSession.getState().model); m.main[0].count = count; s.setModel(m);
    };
    change(121); await s.saveModel();
    const chosenBytes = Array.from(new Uint8Array(await commands.editorReadFile(path)));
    change(122); await s.saveModel();
    const backups = await api.listBackups(path);
    w.__kilnMock.planCommand("editor_restore_backup", { gate: "restore" });
    const calls = w.__kilnMock.calls().filter((c: string) => c === "editor_restore_backup").length;
    const first = s.restoreBackup(backups[0]); // 121, delayed
    const busy = s.useSession.getState().restoring;
    const second = await s.restoreBackup(backups[1]); // 120, must not run
    const switched = await s.openSave(path.replace("character0", "character1"));
    w.__kilnMock.openGate("restore");
    await first;
    const actual = Array.from(new Uint8Array(await commands.editorReadFile(path)));
    return { busy, second, switched, sameBytes: JSON.stringify(actual) === JSON.stringify(chosenBytes),
      calls: w.__kilnMock.calls().filter((c: string) => c === "editor_restore_backup").length - calls,
      name: s.useSession.getState().model.name, count: (await api.openSave(path)).main[0].count };
  });
  expect(result).toEqual({ busy: true, second: false, switched: false, sameBytes: true, calls: 1, name: "Aldric", count: 121 });
});

test("restore confirmation cannot cancel, dismiss or start twice during IPC", async ({ page }) => {
  await open(page);
  await gate(page, "editor_restore_backup", "restore-ui");
  await page.getByRole("button", { name: "Backups", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "Backups for Aldric" });
  const row = dialog.locator(".ke-bk-row").first();
  await row.getByRole("button", { name: "Restore", exact: true }).click();
  await row.getByRole("button", { name: "Restore", exact: true }).click();
  await expect(row.getByRole("button", { name: "Cancel" })).toBeDisabled();
  await expect(row.getByRole("button", { name: /Restor/ })).toBeDisabled();
  await expect(dialog.getByRole("button", { name: "Close", exact: true })).toBeDisabled();
  await page.keyboard.press("Escape");
  await page.locator(".k-modal-backdrop").click({ position: { x: 2, y: 2 } });
  await expect(dialog).toBeVisible();
  if (SHOTS) await page.screenshot({ path: `${SHOTS}/session-restoring.png` });
  await release(page, "restore-ui");
  await expect(dialog).toHaveCount(0);
});

test("Save blocks edits, discard, restores and character changes until the captured bytes are written", async ({ page }) => {
  await open(page);
  await edit(page);
  await gate(page, "editor_write_save", "save");
  await page.getByRole("button", { name: "Save", exact: true }).click();
  const result = await run(page, async () => {
    const w = window as any, s = w.editorSession;
    const before = structuredClone(s.useSession.getState().model);
    const next = structuredClone(before); next.main[0].count = 122; s.setModel(next);
    const count = s.useSession.getState().model.main[0].count;
    s.discard();
    const afterDiscard = s.useSession.getState().model.main[0].count;
    const restored = await s.restoreBackup((await w.editorApi.listBackups(before.path))[0]);
    const switched = await s.openSave(before.path.replace("character0", "character1"));
    return { count, afterDiscard, restored, switched, dirty: s.useSession.getState().dirty };
  });
  expect(result).toEqual({ count: 121, afterDiscard: 121, restored: false, switched: false, dirty: true });
  await expect(page.locator(".k-page-sub")).toHaveText("Saving…");
  await expect(page.locator(".ke-who-btn")).toBeDisabled();
  await expect(page.locator(".ke-body")).toHaveAttribute("inert", "");
  if (SHOTS) await page.screenshot({ path: `${SHOTS}/session-saving.png` });
  await release(page, "save");
  await expect(page.locator(".k-page-sub")).toContainText("Level 25");
  expect(await run(page, async () => {
    const w = window as any, s = w.editorSession, m = s.useSession.getState().model;
    const disk = await w.editorApi.openSave(m.path);
    return { count: disk.main[0].count, dirty: s.useSession.getState().dirty };
  })).toEqual({ count: 121, dirty: false });
  // An edit after completion is accepted and remains dirty, rather than being swallowed by the lock.
  await page.locator('.ke-insp [aria-label="+1"]').first().click();
  await expect(page.locator(".ke-grid .ke-slot.full").first()).toContainText("122");
  await expect(page.locator(".k-page-sub")).toContainText("Changes not saved");
});

test("last selected character wins over an older open response and its error", async ({ page }) => {
  await open(page);
  const result = await run(page, async () => {
    const w = window as any, s = w.editorSession, api = w.editorApi;
    const path = s.useSession.getState().model.path;
    const read = api.openSave;
    let release!: () => void;
    const held = new Promise<void>((r) => { release = r; });
    api.openSave = async (p: string) => { const m = await read(p); if (p.endsWith("character1.ksd")) await held; return m; };
    const first = s.openSave(path.replace("character0", "character1"));
    await s.openSave(path.replace("character0", "character2"));
    release(); await first;
    const name = s.useSession.getState().model.name;
    let reject!: (e: Error) => void;
    api.openSave = () => new Promise((_r, r) => { reject = r; });
    const failed = s.openSave("missing");
    api.openSave = read;
    await s.openSave(path);
    reject(new Error("Old request failed")); await failed;
    return { name, finalName: s.useSession.getState().model.name, status: s.useSession.getState().status };
  });
  expect(result).toEqual({ name: "Mordred", finalName: "Aldric", status: null });
});

test("opening shows feedback and prevents editing the previous character", async ({ page }) => {
  await open(page);
  await gate(page, "editor_read_file", "opening");
  await page.locator(".ke-who-btn").click();
  await page.getByRole("option", { name: /Brienne/ }).click();
  await expect(page.locator(".k-page-sub")).toHaveText("Opening character…");
  await expect(page.locator(".ke-body")).toHaveAttribute("inert", "");
  if (SHOTS) await page.screenshot({ path: `${SHOTS}/session-opening.png` });
  // Selecting a later character remains possible while an open is pending.
  await page.locator(".ke-who-btn").click();
  await page.getByRole("option", { name: /Mordred/ }).click();
  await expect(page.locator(".k-page-title")).toContainText("Mordred");
  await release(page, "opening");
  await expect.poll(() => run(page, () => (window as any).editorSession.useSession.getState().opening)).toBe(false);
  await expect(page.locator(".k-page-title")).toContainText("Mordred");
});

test("Read again asks before discarding; cancelling preserves the draft", async ({ page }) => {
  await open(page);
  await edit(page);
  await page.evaluate(() => (window as any).__kilnMock.planCommand("editor_file_info", { result: { modified: Date.now() + 5000 } }));
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await page.getByRole("button", { name: "Read again" }).click();
  const dialog = page.getByRole("dialog", { name: "Discard the changes?" });
  await expect(dialog).toBeVisible();
  await dialog.getByRole("button", { name: "Cancel" }).click();
  await expect(page.locator(".ke-grid .ke-slot.full").first()).toContainText("121");
  await page.getByRole("button", { name: "Read again" }).click();
  await dialog.getByRole("button", { name: "Discard and open" }).click();
  await expect(page.locator(".ke-grid .ke-slot.full").first()).toContainText("120");
});

test("empty and unreadable pages expose Open file without a title-menu detour", async ({ page }) => {
  await page.goto("/?editor=none#/editor");
  await expect(page.locator(".k-page-sub")).toHaveText("No character saves found.");
  await expect(page.getByRole("button", { name: "Open file…", exact: true })).toBeVisible();
  await page.goto("/#/editor");
  await expect(page.locator(".k-page-title")).toContainText("Aldric");
  await page.locator(".ke-who-btn").click();
  await page.getByRole("option", { name: /Cedric/ }).click();
  await expect(page.locator(".k-page-title")).toContainText("Cedric");
  await expect(page.getByRole("button", { name: "Open file…", exact: true })).toBeVisible();
});

test("character picker ArrowDown moves focus into choices and Escape returns it", async ({ page }) => {
  await open(page);
  const trigger = page.locator(".ke-who-btn");
  await trigger.focus();
  await page.keyboard.press("ArrowDown");
  await expect(page.getByRole("option", { name: /Aldric/ })).toBeFocused();
  await page.keyboard.press("ArrowDown");
  await expect(page.getByRole("option", { name: /Brienne/ })).toBeFocused();
  await page.keyboard.press("Escape");
  await expect(trigger).toBeFocused();
  await expect(page.getByRole("listbox")).toHaveCount(0);
});

test("unsaved edits protect browser reload; cancelling leaves the model intact", async ({ page }) => {
  await open(page);
  expect(await run(page, () => !window.dispatchEvent(new Event("beforeunload", { cancelable: true })))).toBe(false);
  await edit(page);
  expect(await run(page, () => !window.dispatchEvent(new Event("beforeunload", { cancelable: true })))).toBe(true);
  let prompted = false;
  page.once("dialog", async (dialog) => { prompted = dialog.type() === "beforeunload"; await dialog.dismiss(); });
  // Cancelling beforeunload leaves no new load event for reload() to await. Bound that navigation wait, not the
  // test: the assertion below checks that Chromium kept the original document and draft.
  await page.reload({ timeout: 3000 }).catch(() => {});
  expect(prompted).toBe(true);
  await expect(page.locator(".ke-grid .ke-slot.full").first()).toContainText("121");
});

test("Deck Read again asks first and empty state exposes Open file", async ({ page }) => {
  await open(page, true);
  await run(page, () => {
    const s = (window as any).editorSession, m = structuredClone(s.useSession.getState().model);
    m.main[0].count = 121; s.setModel(m); s.setStatus({ tone: "err", again: true, text: "Changed on disk" });
  });
  await page.locator('[data-deck-id="ed-reread"]').click();
  const dialog = page.getByRole("dialog", { name: "Discard the changes?" });
  await expect(dialog).toBeVisible();
  await dialog.getByRole("button", { name: /Cancel/ }).click();
  expect(await run(page, () => (window as any).editorSession.useSession.getState().model.main[0].count)).toBe(121);
  // Clear the draft through the session before the full page navigation.
  await run(page, () => (window as any).editorSession.discard());
  await page.goto("/?ui=deck&editor=none#/deck/editor");
  await expect(page.locator('[data-deck-id="ed-open-file"]')).toBeVisible();
});
