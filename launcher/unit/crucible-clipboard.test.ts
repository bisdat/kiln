// When Crucible reads the clipboard (src/crucible/prefill.ts): never because the launcher started or updated; at start
// only when a pack page's download left a pack waiting; when "Got a code?" opens or Paste is pressed; and only a Crucible
// code or link fills the field.
import { expect, test } from "bun:test";
import type { CrucibleResume } from "../src/api/commands";
import { clipboardPrefill, codePrefill, resumeTarget } from "../src/crucible/prefill";

const LINK = "https://frostdev.io/kiln/crucible/p/M2PD-7FJC";
function reader(text: string | null) {
  const r = { calls: 0, read: async () => { r.calls++; return text; } };
  return r;
}
const start = (over: Partial<CrucibleResume>): CrucibleResume => ({ firstStart: false, code: null, packWaiting: false, ...over });

test("a fresh install from a pack page with the pack waiting reads the clipboard once and opens its pack", async () => {
  const c = reader(LINK);
  expect(await resumeTarget(start({ firstStart: true, packWaiting: true }), c.read)).toBe(LINK);
  expect(c.calls).toBe(1);
});

test("the first start after an update does not read the clipboard", async () => {
  const c = reader(LINK);
  expect(await resumeTarget(start({ firstStart: true }), c.read)).toBeNull();
  expect(c.calls).toBe(0);
});

test("a later start does not read the clipboard", async () => {
  const c = reader(LINK);
  expect(await resumeTarget(start({}), c.read)).toBeNull();
  expect(c.calls).toBe(0);
});

test("the code the download carried wins without reading the clipboard", async () => {
  const c = reader(LINK);
  expect(await resumeTarget(start({ firstStart: true, code: "K4TW9XRB", packWaiting: true }), c.read)).toBe("K4TW9XRB");
  expect(c.calls).toBe(0);
});

test("a waiting pack with other text on the clipboard (or none, or a failed read) opens nothing", async () => {
  expect(await resumeTarget(start({ firstStart: true, packWaiting: true }), reader("a shopping list").read)).toBeNull();
  expect(await resumeTarget(start({ firstStart: true, packWaiting: true }), reader(null).read)).toBeNull();
  expect(await resumeTarget(start({ firstStart: true, packWaiting: true }), async () => { throw new Error("no clipboard"); })).toBeNull();
});

test("opening Got a code? (and Paste) reads the clipboard and fills in its code", async () => {
  const c = reader(LINK);
  expect(await clipboardPrefill(c.read)).toBe("M2PD-7FJC");
  expect(c.calls).toBe(1);
  expect(await clipboardPrefill(reader(" h7qk3mzp ").read)).toBe("H7QK-3MZP");
  expect(await clipboardPrefill(reader("kiln://crucible/pack/ember-run").read)).toBe("kiln://crucible/pack/ember-run");
});

test("text that is not a code or link is ignored", async () => {
  for (const s of ["a shopping list", "hunter2", "https://example.com/p/H7QK-3MZP", "H7QK-3MZ0", "", null]) {
    expect(codePrefill(s)).toBeNull();
    expect(await clipboardPrefill(reader(s).read)).toBeNull();
  }
  expect(await clipboardPrefill(async () => { throw new Error("denied"); })).toBeNull();
});
