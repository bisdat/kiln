// The editor's session, shared by the desktop page (EditorApp.jsx) and the Deck screen (../deck/DeckEditor.jsx): the
// character saves, the open one (`model`) and what it was when read (`orig`), the live state line, the game running,
// and the steps: open, change, undo, redo, discard, save, restore. It lives outside the page, so changes and history
// survive leaving the page or Deck tab. Open/read, restore and successful Save start a fresh history; Discard itself
// is undoable. Reading, checking and writing are upstream's (src/tauri-api.js, src/treecheck.js).
import { create } from 'zustand';
import { api } from '../src/platform.js';
import { editorFileInfo } from '../../api/commands';
import { checkTree, pointsUsed } from '../src/treecheck.js';
import { t } from '../../i18n';
import { t as tv } from '../src/i18n.js';
import { usePendingEdits } from './pending.ts';
import { createHistory, recordHistory, undoHistory, redoHistory, saveHistory, historyDirty, canUndoHistory, canRedoHistory, describeChange, endHistoryGroup, historyShortcut } from './history.js';

// upstream's own messages for a save it cannot read are Portuguese keys; the launcher's tables word them
// (src/i18n/launcher-keys.json), an unknown one stays as it came
const said = (msg) => tv(msg);

export const useSession = create(() => ({
  /** null: still looking */
  saves: null,
  model: null,
  orig: null,
  /** Immutable edit snapshots and the last saved checkpoint, shared by both editor shells. */
  history: null,
  dirty: false,
  /** {tone: 'warn' | 'err', text, again}; `again`: the save changed on disk, the UIs offer "Read again" */
  status: null,
  running: false,
  saving: false,
  restoring: false,
  opening: false,
  /** the save file's time when read: a save the game wrote since is not overwritten */
  readAt: 0,
}));

const set = (p) => {
  useSession.setState(p);
  const s = useSession.getState();
  usePendingEdits.setState({ dirty: s.dirty, busy: mutationsBlocked(s), name: s.model?.name ?? null });
};

export const setStatus = (status) => set({ status });

export async function refreshSaves() {
  try {
    const l = await api.listSaves();
    set({ saves: l.map((s) => (s.error ? { ...s, error: said(s.error) } : s)) });
    return l;
  } catch (e) {
    set({ saves: [], status: { tone: 'err', text: e.message } });
    return [];
  }
}

// Only the latest requested read may replace the session or its status. Saving/restoring exclude new opens;
// opening still permits another choice, but not edits to the character that is about to be replaced.
let openRequest = 0;
export const mutationsBlocked = (s) => s.saving || s.restoring || s.opening;

/** Reads a save; replaces the open one (the UIs ask before dropping changes). */
export function openSave(path) {
  const s = useSession.getState();
  if (s.saving || s.restoring) return Promise.resolve(false);
  return readSave(path);
}

async function readSave(path) {
  const request = ++openRequest;
  set({ opening: true });
  try {
    const m = await api.openSave(path);
    if (request !== openRequest) return false;
    set({ model: m, orig: m, history: createHistory(m), dirty: false, readAt: m.modified ?? 0,
      // a save read with unknown items says "read-only" by itself; one that could not be read at all gets the word
      status: m.error ? { tone: 'warn', text: m.readOnly ? t('editor.page.readOnlyError', { erro: said(m.error) }) : said(m.error) } : null });
    return true;
  } catch (e) {
    if (request === openRequest) set({ status: { tone: 'err', text: e.message } });
    return false;
  } finally {
    if (request === openRequest) set({ opening: false });
  }
}

/** A save read only by accepting unknown item ids (a mod that is gone) stays read-only: writing refuses those ids. */
export const readOnly = (m) => !!(m && (m.readOnly || m.error));

/** A change to the open model (the views call it with the whole new model). A read-only save shows what it holds and
 *  takes no change: nothing of it could be written. */
export function setModel(m) {
  const s = useSession.getState();
  if (!s.model || !m || mutationsBlocked(s) || readOnly(s.model) || m.path !== s.model.path) return false;
  applyModel(m);
  return true;
}

// One commit path for user changes (including Discard). Components keep passing immutable whole models.
let typingGesture = null;
function applyModel(m, action) {
  const s = useSession.getState();
  const history = recordHistory(s.history, m, { ...(action ?? describeChange(s.model, m)), gesture: typingGesture });
  set({ model: history.present, history, dirty: historyDirty(history) });
}

export const canUndo = (s) => !readOnly(s.model) && canUndoHistory(s.history, s);
export const canRedo = (s) => !readOnly(s.model) && canRedoHistory(s.history, s);
export const historyLabel = (s, direction) => {
  const step = s.history?.[direction === 'undo' ? 'past' : 'future'].at(-1);
  return step ? t(`editor.page.history.${step.label.key}`, { count: step.label.count }) : '';
};
export const historyTitle = (s, direction) => {
  const action = historyLabel(s, direction);
  return action ? t(`editor.page.history.${direction}Action`, { action }) : t(`editor.page.history.${direction === 'undo' ? 'emptyUndo' : 'emptyRedo'}`);
};
function travel(direction) {
  const s = useSession.getState();
  if (!(direction === 'undo' ? canUndo(s) : canRedo(s))) return false;
  const history = direction === 'undo' ? undoHistory(s.history, s) : redoHistory(s.history, s);
  set({ history, model: history.present, dirty: historyDirty(history) });
  return true;
}
export const undo = () => travel('undo');
export const redo = () => travel('redo');

/** Shell-level listeners keep typing (even across pauses) in one gesture. Blur/navigation ends the group. */
export function startHistoryControls() {
  const boundary = () => {
    typingGesture = null;
    const s = useSession.getState();
    if (s.history?.group) set({ history: endHistoryGroup(s.history) });
  };
  const focus = (e) => {
    boundary();
    if (e.target.matches?.('input, textarea, [contenteditable="true"]')) typingGesture = {};
  };
  const key = (e) => historyShortcut(e, undo, redo);
  window.addEventListener('focusin', focus);
  window.addEventListener('focusout', boundary);
  window.addEventListener('keydown', key);
  return () => {
    boundary();
    window.removeEventListener('focusin', focus);
    window.removeEventListener('focusout', boundary);
    window.removeEventListener('keydown', key);
  };
}

export function discard() {
  const s = useSession.getState();
  if (mutationsBlocked(s)) return;
  if (s.dirty) { applyModel(s.orig, { label: { key: 'discard' } }); set({ status: null }); }
}

/** Reads the open character again when its save was written since it was read (the player went to play and came
 *  back) and nothing here is changed. With changes the open character stays: Save then refuses and offers "Read again". */
async function rereadIfWritten() {
  const s = useSession.getState();
  if (!s.model || s.dirty || mutationsBlocked(s) || !s.readAt) return;
  const path = s.model.path;
  try {
    const { modified } = await editorFileInfo(path);
    const now = useSession.getState();
    if (modified && modified !== now.readAt && now.model?.path === path && !now.dirty && !mutationsBlocked(now)) await openSave(path);
  } catch { /* the file is gone or cannot be read: the open character stays as it was read */ }
}

/** A visit of the page or the Deck tab: the list, then the newest readable character unless one is open already (an
 *  open one without changes is read again when the game wrote its save meanwhile, also when the game quits); the
 *  game's state every 3 s. Returns the stop function. */
export function startSession() {
  refreshSaves().then((l) => {
    const s = useSession.getState();
    if (s.model || mutationsBlocked(s)) return;
    const best = [...l].filter((x) => !x.error).sort((a, b) => b.modified - a.modified)[0];
    if (best) openSave(best.path);
  });
  rereadIfWritten();
  const poll = () => api.gameRunning().then((running) => {
    const was = useSession.getState().running;
    if (running === was) return;
    set({ running });
    if (was && !running) rereadIfWritten();
  }).catch(() => {});
  poll();
  const timer = setInterval(poll, 3000);
  return () => clearInterval(timer);
}

/** Reads the open character again when nothing is changed (new item tables: a mod's items merged). */
export function reopenIfClean() {
  const s = useSession.getState();
  if (s.model && !s.dirty && !mutationsBlocked(s)) return openSave(s.model.path);
  return Promise.resolve(false);
}

export const canSave = (s) => !!(s.dirty && !s.running && s.model && !readOnly(s.model) && !mutationsBlocked(s));

/** Writes the open model. Returns 'saved', or what stopped it: 'skills' (the tree checks), 'changed' (the game wrote the
 *  save since it was read), 'error'. */
export async function saveModel() {
  const s = useSession.getState();
  if (!canSave(s)) return 'error';
  const { model, orig } = s;
  // a tree the game finds invalid is refunded whole on load: refuse before writing
  if (JSON.stringify(model.skills) !== JSON.stringify(orig.skills)) {
    const { problems, used, total } = checkTree(model);
    if (total === null && used > pointsUsed(orig.skills)) {
      problems.push(t('editor.page.pointsRaised', { antes: pointsUsed(orig.skills), depois: used }));
    }
    if (problems.length) { set({ status: { tone: 'err', text: t('editor.page.notWritten', { problemas: problems.join(' · ') }) } }); return 'skills'; }
  }
  set({ saving: true });
  try {
    // the game may have written the save since it was read (played after opening the editor): writing the old
    // character over it would lose that play
    const { modified } = await editorFileInfo(model.path);
    if (s.readAt && modified && modified !== s.readAt) {
      set({ saving: false, status: { tone: 'err', again: true, text: t('editor.page.gameWroteSince') } });
      return 'changed';
    }
    const { model: m } = await api.writeSave(model.path, model);
    // Adopt the writer's normalized slots and mask. History starts afresh at this serialization boundary.
    set({ model: m, orig: m, history: saveHistory(s.history, m), dirty: false, readAt: m.modified ?? 0, status: null, saving: false });
    refreshSaves();
    return 'saved';
  } catch (e) {
    set({ saving: false, status: { tone: 'err', text: e.message } });
    return 'error';
  }
}

/** Restores a backup over the open save and reads it again (the save before it is kept as a backup). */
export async function restoreBackup(b) {
  const s = useSession.getState();
  if (!s.model || s.running || mutationsBlocked(s)) return false;
  const target = s.model.path;
  set({ restoring: true });
  try {
    await api.restoreBackup(b.path, target);
    return await readSave(target);
  } finally {
    set({ restoring: false });
  }
}
