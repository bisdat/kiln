import { expect, test } from "bun:test";
import { CODE_ALPHABET, codeInput, codeOf, countsOf, anyChange, coverIndex, crucibleTarget, normalizeCode } from "../src/crucible/code";

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
