import { expect, test } from "bun:test";
import { CODE_ALPHABET, checksLine, codeInput, codeOf, countsOf, anyChange, coverIndex, crucibleTarget, normalizeCode, shareEstimate, uniqueName } from "../src/crucible/code";
import type { LibraryEntry, Profile } from "../src/api/commands";

test("codes: 8 symbols of the alphabet, any case, with or without the dash", () => {
  expect(normalizeCode("h7qk-3mzp")).toBe("H7QK-3MZP");
  expect(normalizeCode(" H7QK 3MZP ")).toBe("H7QK-3MZP");
  expect(normalizeCode("H7QK3MZP")).toBe("H7QK-3MZP");
  // look-alikes are not in the alphabet
  expect(normalizeCode("H7QK-3MZ0")).toBeNull();
  expect(normalizeCode("I7QK-3MZP")).toBeNull();
  expect(normalizeCode("H7QK-3MZ")).toBeNull();
  expect(CODE_ALPHABET).not.toMatch(/[01ILO]/);
});

test("the code field: capitals, the dash after four, at most eight symbols", () => {
  expect(codeInput("h7q")).toBe("H7Q");
  expect(codeInput("h7qk3")).toBe("H7QK-3");
  expect(codeInput("h7qk-3mzp99")).toBe("H7QK-3MZP");
  expect(codeInput("o0il")).toBe("");
});

test("targets: codes, share links, kiln:// links, version ids; nothing else", () => {
  expect(crucibleTarget("h7qk-3mzp")).toBe("H7QK-3MZP");
  expect(crucibleTarget("https://frostdev.io/kiln/crucible/p/H7QK-3MZP")).toBe("https://frostdev.io/kiln/crucible/p/H7QK-3MZP");
  expect(crucibleTarget("frostdev.io/kiln/crucible/ember-run")).toBe("https://frostdev.io/kiln/crucible/ember-run");
  expect(crucibleTarget("kiln://crucible/H7QK-3MZP")).toBe("kiln://crucible/H7QK-3MZP");
  expect(crucibleTarget("kiln://crucible/pack/ember-run")).toBe("kiln://crucible/pack/ember-run");
  expect(crucibleTarget("pv_abc123")).toBe("pv_abc123");
  expect(crucibleTarget("hello there")).toBeNull();
  expect(crucibleTarget("https://example.com/p/H7QK-3MZP")).toBeNull();
  expect(crucibleTarget("")).toBeNull();
  expect(codeOf("https://frostdev.io/kiln/crucible/p/H7QK-3MZP")).toBe("H7QK-3MZP");
  expect(codeOf("kiln://crucible/pack/ember-run")).toBeNull();
});

test("covers: the same pack gets the same cover", () => {
  expect(coverIndex("pk_cinder")).toBe(coverIndex("pk_cinder"));
  const seen = new Set(["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"].map(coverIndex));
  expect(seen.size).toBeGreaterThan(2);
  for (const i of seen) expect(i).toBeLessThan(6);
});

test("change counts", () => {
  const c = countsOf({ added: ["a"], updated: ["b", "c"], removed: [] });
  expect(c).toEqual({ added: 1, updated: 2, removed: 0 });
  expect(anyChange(c)).toBe(true);
  expect(anyChange(countsOf(null))).toBe(false);
});

test("an installed pack's name: never one a profile has (any case), a number added, at most 128 characters", () => {
  const taken = ["My mods", "my mods (2)", "Ember run (Emberfox's pack)"];
  expect(uniqueName("My mods", taken)).toBe("My mods (3)");
  expect(uniqueName("  MY MODS ", taken)).toBe("MY MODS (3)");
  expect(uniqueName("Ember run (Emberfox's pack)", taken)).toBe("Ember run (Emberfox's pack) (2)");
  expect(uniqueName("Cinder Trails", taken)).toBe("Cinder Trails");
  const long = "x".repeat(130);
  const n = uniqueName(long, [long.slice(0, 128)]);
  expect(Array.from(n)).toHaveLength(128);
  expect(n.endsWith(" (2)")).toBe(true);
});

test("what a first share sends: every mod but the player layer, the Library's sizes", () => {
  const mods = { kiln: [{ id: "lantern-glow", version: "1.2.0", hash: "kdh1:a", enabled: true }, { id: "night-sky", version: "0.3.1", hash: "kdh1:b", enabled: false },
    { id: "local.my-mods", version: "1.0.0", hash: "kdh1:l", enabled: true }], native: [{ id: "fps-unlock", version: "1.0.0", hash: "sha:c", enabled: true }] };
  const profile = { id: "my-mods", name: "My mods", mods } as unknown as Profile;
  const v = (version: string, hash: string, size: number) => ({ version, hash, size });
  const library = [
    { kind: "kiln", id: "lantern-glow", name: "Lantern Glow", versions: [v("1.1.0", "kdh1:old", 5), v("1.2.0", "kdh1:a", 1000)] },
    { kind: "kiln", id: "night-sky", name: "Night Sky", versions: [v("0.3.1", "kdh1:other", 200)] },
    { kind: "native", id: "fps-unlock", name: "FPS Unlock", versions: [v("1.0.0", "sha:c", 30)] },
  ] as unknown as LibraryEntry[];
  // night-sky is off (it goes as optional, off) and matched by its version when the hash differs
  expect(shareEstimate(profile, library)).toEqual({ mods: 3, bytes: 1230 });
  expect(shareEstimate(profile, undefined)).toBeNull();
  expect(shareEstimate(undefined, library)).toBeNull();
});

test("the preview's check line from Crucible's report", () => {
  expect(checksLine(null)).toBeNull();
  expect(checksLine(undefined)).toBeNull();
  expect(checksLine({ state: "done", ok: true, errors: [], warnings: [] })?.kind).toBe("passed");
  // a Crucible without the check worker: the quick checks are all there is
  expect(checksLine({ state: "off", ok: true, errors: [], warnings: [] })?.kind).toBe("passed");
  expect(checksLine({ state: "done", errors: [], warnings: ["a", "b"] })).toEqual({ kind: "warnings", count: 2, messages: ["a", "b"] });
  for (const state of ["queued", "running", "waiting"] as const) expect(checksLine({ state, errors: [], warnings: ["quick"] })?.kind).toBe("pending");
  // problems the quick checks found show while the full check runs
  expect(checksLine({ state: "running", errors: ["Taken down from Crucible: x"] })).toEqual({ kind: "problems", count: 1, messages: ["Taken down from Crucible: x"] });
  expect(checksLine({ state: "failed" })?.kind).toBe("failed");
  expect(checksLine({ state: "done", errors: "x".repeat(7).split("") }).messages).toHaveLength(5);
});
