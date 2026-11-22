import { describe, expect, test } from "bun:test";
// @ts-ignore: the mounted editor is plain JavaScript
import { createHistory, recordHistory, undoHistory, redoHistory, saveHistory, historyDirty, canUndoHistory, canRedoHistory, endHistoryGroup, describeChange } from "../src/editor/kiln/history.js";

const model = () => ({ path: "character0.ksd", level: 25, xp: 1200, main: [{ slot: 0, itemId: 1, count: 120 }], extra: [], equipment: [], skills: [], knowledge: Array.from({ length: 2000 }, (_, id) => ({ id, value: 1 })) });
const amount = (m: ReturnType<typeof model>, count: number) => ({ ...m, main: [{ ...m.main[0], count }] });
const record = (h: any, m: any, time = 0) => recordHistory(h, m, { ...describeChange(h.present, m), time });

describe("editor history", () => {
  test("undo and redo restore exact immutable snapshots and unchanged branches are shared", () => {
    const original = model(), h = record(createHistory(original), amount(original, 121));
    expect(h.present.knowledge).toBe(original.knowledge);
    expect(h.present.main[0]).not.toBe(original.main[0]);
    expect(original.main[0].count).toBe(120);
    const undone = undoHistory(h);
    expect(undone.present).toBe(original);
    expect(redoHistory(undone).present).toBe(h.present);
  });
  test("rapid changes of one field group, another field and a pause do not", () => {
    let h = createHistory(model());
    h = record(h, amount(h.present, 121), 0);
    h = record(h, amount(h.present, 122), 100);
    h = record(h, amount(h.present, 123), 700);
    expect(h.past).toHaveLength(1);
    expect(undoHistory(h).present.main[0].count).toBe(120);
    h = record(h, { ...h.present, level: 26 }, 710);
    h = record(h, amount(h.present, 124), 1500);
    expect(h.past).toHaveLength(3);
  });
  test("one focused typing gesture stays grouped across pauses; blur ends it", () => {
    let h = createHistory(model());
    const change = (count: number, time: number) => { const m = amount(h.present, count); h = recordHistory(h, m, { ...describeChange(h.present, m), time, gesture: "field-1" }); };
    change(1, 0); change(12, 2000); change(125, 4000);
    expect(h.past).toHaveLength(1);
    h = endHistoryGroup(h);
    change(126, 4100);
    expect(h.past).toHaveLength(2);
  });
  test("discrete bulk actions never merge and carry an action count", () => {
    const m = model(); m.main.push({ slot: 1, itemId: 2, count: 1 }, { slot: 2, itemId: 3, count: 1 });
    let h = createHistory(m);
    const next = { ...m, main: [] };
    expect(describeChange(m, next).label).toEqual({ key: "removeItems", count: 3 });
    h = record(h, next); h = record(h, { ...next, knowledge: [] }, 1);
    expect(h.past).toHaveLength(2);
    expect(undoHistory(undoHistory(h)).present).toBe(m);
  });
  test("history is capped, deep-cloned callers are structurally shared, no-ops do not erase redo", () => {
    let h = createHistory(model(), 100);
    const knowledge = h.present.knowledge;
    for (let i = 0; i < 150; i++) h = record(endHistoryGroup(h), amount(structuredClone(h.present), 121 + i), i * 1000);
    expect(h.past).toHaveLength(100);
    expect(h.present.knowledge).toBe(knowledge);
    for (const step of h.past) expect(step.model.knowledge).toBe(knowledge);
    h = undoHistory(h);
    expect(record(h, structuredClone(h.present))).toBe(h);
    expect(h.future).toHaveLength(1);
    h = record(h, amount(h.present, 999));
    expect(h.future).toHaveLength(0);
  });
  test("dirty follows the saved snapshot; a successful save adopts normalized data and starts fresh", () => {
    const original = model(); let h = createHistory(original);
    h = record(h, amount(h.present, 121)); h = endHistoryGroup(h);
    h = record(h, amount(h.present, 122));
    expect(historyDirty(h)).toBe(true);
    h = undoHistory(undoHistory(h));
    expect(historyDirty(h)).toBe(false);
    expect(historyDirty(redoHistory(h))).toBe(true);
    // Independently reconstructing the saved state is also clean.
    expect(historyDirty(record(redoHistory(h), structuredClone(h.saved)))).toBe(false);
    h = redoHistory(h);
    const normalized = { ...h.present, modified: 12345 };
    h = saveHistory(h, normalized);
    expect(h.present).toBe(normalized);
    expect(historyDirty(h)).toBe(false);
    expect(h.past).toHaveLength(0); expect(h.future).toHaveLength(0);
    h = record(h, amount(h.present, 125));
    expect(historyDirty(undoHistory(h))).toBe(false);
    expect(historyDirty(redoHistory(undoHistory(h)))).toBe(true);
  });
  test("save, restore and open busy guards disable undo and redo without consuming history", () => {
    const h = record(createHistory(model()), amount(model(), 121));
    for (const busy of ["saving", "restoring", "opening"]) {
      const state = { [busy]: true };
      expect(canUndoHistory(h, state)).toBe(false);
      expect(undoHistory(h, state)).toBe(h);
      const undone = undoHistory(h);
      expect(canRedoHistory(undone, state)).toBe(false);
      expect(redoHistory(undone, state)).toBe(undone);
    }
  });
  test("Knowledge removals share surviving entries even when their array indices shift", () => {
    const original = model();
    const next = { ...original, knowledge: structuredClone(original.knowledge.slice(1)) };
    const h = record(createHistory(original), next);
    expect(h.present.knowledge[0]).toBe(original.knowledge[1]);
    expect(h.present.knowledge.at(-1)).toBe(original.knowledge.at(-1));
  });
  test("a fresh read/restore session resets both stacks and the saved checkpoint", () => {
    let h = record(createHistory(model()), amount(model(), 121));
    h = undoHistory(h);
    h = createHistory({ ...model(), path: "character1.ksd" });
    expect(h.past).toHaveLength(0); expect(h.future).toHaveLength(0);
    expect(historyDirty(h)).toBe(false);
    expect(canUndoHistory(h)).toBe(false); expect(canRedoHistory(h)).toBe(false);
  });
});
