import { expect, test } from "bun:test";
import { stopFilter, stopIds, stopKey, stopSentence } from "../src/shell/stoppedStart";
import { filterRows, type ModRow } from "../src/shell/mods";
import type { StoppedStart } from "../src/api/commands";
const stop: StoppedStart = { kind: "roster", reason: "128 entries", mods: ["a"], names: ["A"], advice: "Switch one off", utc: "2026-11-15T12:00:00Z", pid: 42, exitCode: 19202 };
test("stop identity includes time to distinguish a reused pid", () => {
  expect(stopKey(stop)).not.toBe(stopKey({ ...stop, utc: "2026-11-16T12:00:00Z" }));
  expect(stopSentence(stop)).toContain("Survivors roster than the game can hold: A");
  expect(stopSentence({ ...stop, kind: "boot-mod" })).toBe("128 entries (A)");
});
test("the existing mods filter selects all named ids exactly, including punctuation", () => {
  const ids = ["a", "b,c", "c:d"];
  expect(stopIds(stopFilter(ids))).toEqual(ids);
  const rows = [...ids, "aa", "other"].map((id) => ({ id, name: id, kind: "kiln" })) as ModRow[];
  expect(filterRows(rows, stopFilter(ids), "").map((r) => r.id)).toEqual(ids);
  expect(filterRows(rows, "other", "").map((r) => r.id)).toEqual(["other"]);
  expect(stopIds("ids:%oops")).toEqual([]);
});

import { mockStoppedFile, readMockStop } from "../src/api/mockStoppedStart";
test("reading rules: current roster and required mod, unknown, malformed, stale, quit, and bounded input", () => {
  for (const kind of ["roster", "boot-mod"] as const)
    expect(readMockStop(mockStoppedFile(kind, 42, 1000), 42, 900, 1100)?.kind).toBe(kind);
  for (const kind of ["unknown", "garbled", "stale", "quit"] as const)
    expect(readMockStop(mockStoppedFile(kind, 42, 1000), 42, 900, 1100)).toBeNull();
  const file = mockStoppedFile("roster", 42, 1000);
  expect(readMockStop(file, 43, 900, 1100)).toBeNull();
  expect(readMockStop(file, 42, 1001, 1100)).toBeNull();
  expect(readMockStop(file, 42, 900, 999)).toBeNull();
  expect(readMockStop(" ".repeat(256 * 1024) + file, 42, 900, 1100)).toBeNull();
});
