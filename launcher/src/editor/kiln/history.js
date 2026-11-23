// Immutable editor snapshots. Unchanged branches are shared even when a caller rebuilt a whole model; at most
// 100 transitions plus the saved checkpoint are retained. Labels are translation keys, not captured UI language.
const GROUP_MS = 750;
const busy = (s) => !!(s?.saving || s?.restoring || s?.opening);

/** Reuse equal branches, without serializing or copying every large Knowledge list into every history entry. */
function share(before, next) {
  if (Object.is(before, next)) return before;
  if (!before || !next || typeof before !== 'object' || typeof next !== 'object' || Array.isArray(before) !== Array.isArray(next)) return next;
  const keys = Object.keys(next), oldKeys = Object.keys(before);
  let same = keys.length === oldKeys.length;
  const result = Array.isArray(next) ? [] : {};
  // Removing one Knowledge entry shifts all later indices; keep those entry objects shared by their stable id.
  const idKey = Array.isArray(next) && next.length && (Object.hasOwn(next[0] ?? {}, 'id') ? 'id' : Object.hasOwn(next[0] ?? {}, 'slot') ? 'slot' : null);
  const byId = idKey ? new Map(before.map((entry) => [entry[idKey], entry])) : null;
  for (const key of keys) {
    result[key] = share(byId ? byId.get(next[key][idKey]) : before[key], next[key]);
    if (!Object.hasOwn(before, key) || result[key] !== before[key]) same = false;
  }
  return same ? before : result;
}
const equal = (a, b) => share(a, b) === a;

export function createHistory(model, limit = 100) {
  return { present: model, saved: model, past: [], future: [], group: null, limit };
}
export const historyDirty = (h) => !!h && !equal(h.present, h.saved);
export const canUndoHistory = (h, state) => !!h?.past.length && !busy(state);
export const canRedoHistory = (h, state) => !!h?.future.length && !busy(state);
export const endHistoryGroup = (h) => h?.group ? { ...h, group: null } : h;
// A write can densely renumber equipment slots. The fresh parsed model is a new serialization baseline, so
// earlier snapshots must not be replayed against it. Undo/redo are explicitly scoped to edits since the last save.
export const saveHistory = (h, model = h.present) => createHistory(model, h.limit);

export function recordHistory(h, model, { label = { key: 'change' }, group = null, time = Date.now(), gesture = null } = {}) {
  const present = share(h.present, model);
  if (present === h.present) return h;
  const merge = group && h.group?.key === group &&
    (gesture ? gesture === h.group.gesture : !h.group.gesture && time - h.group.time <= GROUP_MS);
  const past = merge ? h.past : [...h.past, { model: h.present, label }].slice(-h.limit);
  return { ...h, present, past, future: [], group: group ? { key: group, time, gesture } : null };
}
export function undoHistory(h, state) {
  if (!canUndoHistory(h, state)) return h;
  const step = h.past.at(-1);
  return { ...h, present: step.model, past: h.past.slice(0, -1), future: [...h.future, { model: h.present, label: step.label }], group: null };
}
export function redoHistory(h, state) {
  if (!canRedoHistory(h, state)) return h;
  const step = h.future.at(-1);
  return { ...h, present: step.model, past: [...h.past, { model: h.present, label: step.label }], future: h.future.slice(0, -1), group: null };
}

/** Infer one short action at the model funnel. Discrete list edits never coalesce; only the same scalar field can. */
export function describeChange(before, next) {
  const action = (key, group = null, count) => ({ label: { key, ...(count === undefined ? {} : { count }) }, group });
  if (before.level !== next.level) return action('changeLevel', 'level');
  if (before.xp !== next.xp) return action('changeXp', 'xp');
  if (!equal(before.knowledge, next.knowledge)) {
    const removed = (before.knowledge?.length ?? 0) - (next.knowledge?.length ?? 0);
    return removed > 0 ? action('removeKnowledge', null, removed) : action('changeKnowledge');
  }
  if (!equal(before.skills, next.skills)) {
    const ids = new Set([...before.skills, ...next.skills].map((s) => s.id));
    const changed = [...ids].filter((id) => !equal(before.skills.find((s) => s.id === id), next.skills.find((s) => s.id === id)));
    return action('changeSkills', changed.length === 1 ? `skill:${changed[0]}` : null);
  }
  const lists = ['main', 'extra', 'equipment'];
  const count = (m) => lists.reduce((n, key) => n + m[key].length, 0);
  const delta = count(next) - count(before);
  if (delta < 0) return action('removeItems', null, -delta);
  if (delta > 0) return action('addItems', null, delta);
  const changed = [];
  for (const list of lists) for (const entry of next[list]) {
    const old = before[list].find((e) => e.slot === entry.slot);
    if (!equal(old, entry)) changed.push({ list, entry, old });
  }
  if (changed.length === 1 && changed[0].old?.itemId === changed[0].entry.itemId) {
    const { list, entry, old } = changed[0];
    const fields = [...new Set([...Object.keys(old), ...Object.keys(entry)])].filter((k) => !equal(old[k], entry[k]));
    const key = fields.length === 1 && fields[0] === 'count' ? 'changeAmount' : 'changeItem';
    return action(key, `${list}:${entry.slot}:${entry.itemId}:${fields.sort().join(',')}`);
  }
  return action(changed.length > 1 ? 'moveItems' : 'change');
}

/** Text controls keep their own undo stack, including contenteditable and controls inside a dialog. */
export function historyShortcut(event, undo, redo) {
  if (!event.ctrlKey || event.altKey || event.metaKey || event.defaultPrevented ||
      event.target?.closest?.('input, textarea, select, [contenteditable]:not([contenteditable="false"]), [role="textbox"], [role="dialog"]')) return false;
  const key = event.key.toLowerCase();
  if (key !== 'z' && key !== 'y') return false;
  event.preventDefault();
  if (key === 'y' || event.shiftKey) redo(); else undo();
  return true;
}
