// Inbox 90 (29.18): an add's toast says "Updated" for another version of a mod the profile had, with both versions
// where there is room, and what it said before for a new mod or the same version again (src/shell/addWords.ts).
import { expect, test } from "bun:test";
import type { AddReport, AddedMod, ProfileMod } from "../src/api/commands";

const store = new Map<string, string>();
(globalThis as { localStorage?: unknown }).localStorage ??= {
  getItem: (k: string) => store.get(k) ?? null,
  setItem: (k: string, v: string) => void store.set(k, v),
  removeItem: (k: string) => void store.delete(k),
};
const { addTitle, allUpdated, deckAddTitle, splitAdded } = await import("../src/shell/addWords");

const mod = (id: string, name: string, version: string): AddedMod => ({ kind: "kiln", id, version, hash: `h-${id}-${version}`, name });
const pm = (m: AddedMod, enabled = true): ProfileMod => ({ id: m.id, version: m.version, hash: m.hash, enabled });
type R = Pick<AddReport, "added" | "changes">;
/** An add of `after`, the profile having had `before` (null: new). */
const add = (...xs: [AddedMod, ProfileMod | null][]): R => ({ added: xs.map(([m]) => m), changes: xs.map(([m, before]) => ({ kind: m.kind, before, after: pm(m) })) });

const owl12 = mod("night-owl", "Night Owl", "1.2.0"), owl13 = mod("night-owl", "Night Owl", "1.3.0");
const lamp1 = mod("dawn-lamp", "Dawn Lamp", "1.0.0"), lamp2 = mod("dawn-lamp", "Dawn Lamp", "2.0.0");
const fog = mod("fog", "Fog", "0.1.0");

test("a mod new in the profile is Added, as before", () => {
  expect(splitAdded(add([owl12, null]))).toEqual({ added: [owl12], updated: [] });
  expect(addTitle(add([owl12, null]))).toBe("Added Night Owl");
  expect(deckAddTitle(add([owl12, null]))).toBe("Added Night Owl");
  expect(addTitle(add([owl12, null], [lamp1, null]))).toBe("Added Night Owl and Dawn Lamp");
  expect(deckAddTitle(add([owl12, null], [lamp1, null], [fog, null]))).toBe("Added 3 mods");
  expect(allUpdated(add([owl12, null]))).toBe(false);
});

test("another version of a mod the profile had is Updated, with both versions", () => {
  const r = add([owl13, pm(owl12)]);
  expect(splitAdded(r)).toEqual({ added: [], updated: [{ mod: owl13, from: "1.2.0" }] });
  expect(addTitle(r)).toBe("Updated Night Owl: 1.2.0 → 1.3.0");
  expect(deckAddTitle(r)).toBe("Updated Night Owl: 1.2.0 → 1.3.0");
  expect(allUpdated(r)).toBe(true);
  // one that was off in the profile in its old version: still an update
  expect(addTitle(add([owl13, pm(owl12, false)]))).toBe("Updated Night Owl: 1.2.0 → 1.3.0");
  // an older file is the other way round, and says so
  expect(addTitle(add([owl12, pm(owl13)]))).toBe("Updated Night Owl: 1.3.0 → 1.2.0");
});

test("the same version again says what it said before: Added (switched back on, or the files changed)", () => {
  expect(addTitle(add([owl12, pm(owl12, false)]))).toBe("Added Night Owl");
  expect(addTitle(add([owl12, { ...pm(owl12), hash: "other" }]))).toBe("Added Night Owl");
  expect(deckAddTitle(add([owl12, pm(owl12, false)]))).toBe("Added Night Owl");
});

test("versions too long for a title are left out; several updates and a mix are named", () => {
  const long = mod("night-owl", "Night Owl", "1.3.0-dev+20261118.abcdef0");
  expect(addTitle(add([long, pm(owl12)]))).toBe("Updated Night Owl");
  expect(deckAddTitle(add([long, pm(owl12)]))).toBe("Updated Night Owl");
  expect(addTitle(add([owl13, pm(owl12)], [lamp2, pm(lamp1)]))).toBe("Updated Night Owl and Dawn Lamp");
  expect(deckAddTitle(add([owl13, pm(owl12)], [lamp2, pm(lamp1)]))).toBe("Updated 2 mods");
  expect(addTitle(add([fog, null], [owl13, pm(owl12)]))).toBe("Added Fog, updated Night Owl");
  expect(deckAddTitle(add([fog, null], [owl13, pm(owl12)]))).toBe("1 added, 1 updated");
  expect(allUpdated(add([fog, null], [owl13, pm(owl12)]))).toBe(false);
});

test("a change that switched another mod off is no add of its own", () => {
  // quick::merge: a second copy of the same DLL switched off is in `changes`, not in `added`
  const off = mod("other-copy", "Other copy", "1.0.0");
  const r: R = { added: [owl13], changes: [{ kind: "kiln", before: pm(owl12), after: pm(owl13) }, { kind: "kiln", before: pm(off), after: pm(off, false) }] };
  expect(addTitle(r)).toBe("Updated Night Owl: 1.2.0 → 1.3.0");
});
