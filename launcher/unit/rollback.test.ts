// Earlier versions kept for a roll back (src/shell/rollback.ts, kiln_manage::kept): what a mod's menu offers ("Roll
// back to" for a kept version, "Use" for the others), the Library card's kept list and counts, and the toast's words.
import { expect, test } from "bun:test";
import type { KeptInfo, LibraryEntry, LibraryVersion, RollbackReport } from "../src/api/commands";
import { keptSize, keptVersions, liveVersions, otherVersions, rollbackLine, rollbackTargets } from "../src/shell/rollback";

const hash = (s: string) => `kdh1:${s.padEnd(64, "0").slice(0, 64)}`;
const kept = (at: string, settings: string[] = []): KeptInfo => ({ kept: at, replacedBy: "2.0.0", settings, settingsSize: settings.length * 10 });
const ver = (v: string, k: KeptInfo | null = null, seed = v): LibraryVersion => ({
  version: v, hash: hash(seed), folder: `library/greeneye/map/${v}`, imported: "2026-11-01T00:00:00Z", side: "client", clients: "none",
  size: 1000, source: null, manifestId: null, manifest: null, kept: k,
});
const entry = (id: string, versions: LibraryVersion[], name = id): LibraryEntry => ({ kind: "greeneye", id, name, versions });

test("a kept version is offered as Roll back to, never as Use, and never the one in use", () => {
  const e = entry("map", [ver("2.0.0"), ver("1.0.0", kept("2026-11-10T00:00:00Z")), ver("0.9.0", kept("2026-11-12T00:00:00Z"))]);
  const inUse = { version: "2.0.0", hash: hash("2.0.0") };
  // newest kept first
  expect(rollbackTargets(e, inUse).map((v) => v.version)).toEqual(["0.9.0", "1.0.0"]);
  expect(otherVersions(e, "2.0.0")).toEqual([]);
  // after a roll back to 1.0.0 the entry uses a version that is no longer kept; 2.0.0 is the kept one
  const after = entry("map", [ver("1.0.0"), ver("2.0.0", kept("2026-11-14T00:00:00Z"))]);
  expect(rollbackTargets(after, { version: "1.0.0", hash: hash("1.0.0") }).map((v) => v.version)).toEqual(["2.0.0"]);
  // a mod the profile does not list has nothing to roll back in it; a mod without kept versions neither
  expect(rollbackTargets(entry("map", [ver("1.0.0")]), inUse)).toEqual([]);
  expect(rollbackTargets(null, null)).toEqual([]);
});

test("two builds of one version: the kept one is told apart by its hash", () => {
  const e = entry("fly", [ver("0.0.0", null, "new"), ver("0.0.0", kept("2026-11-14T00:00:00Z"), "old")]);
  expect(rollbackTargets(e, { version: "0.0.0", hash: hash("new") }).map((v) => v.hash)).toEqual([hash("old")]);
  expect(rollbackTargets(e, { version: "0.0.0", hash: hash("old") })).toEqual([]);
});

test("the Library card lists every kept version with its size, and does not count it as a second copy", () => {
  const lib = [
    entry("map", [ver("2.0.0"), ver("1.0.0", kept("2026-11-10T00:00:00Z", ["greeneye/map/minimap_config.ini"]))], "The Worst Map"),
    entry("stacks", [ver("3.0.0"), ver("2.5.0")], "Big Stacks"),
    entry("fly", [ver("1.1.0"), ver("1.0.0", kept("2026-11-12T00:00:00Z"))], "Fly"),
  ];
  expect(keptVersions(lib).map((k) => `${k.name} ${k.version.version}`)).toEqual(["Fly 1.0.0", "The Worst Map 1.0.0"]);
  expect(keptSize(lib[0].versions[1])).toBe(1010);
  expect(lib.map(liveVersions)).toEqual([1, 2, 1]);
  expect(lib.filter((e) => liveVersions(e) > 1).map((e) => e.id)).toEqual(["stacks"]);
});

test("the toast says what comes back and what is kept", () => {
  const r: RollbackReport = {
    profileId: "my-mods", profileName: "My mods", kind: "greeneye", id: "map", name: "The Worst Map",
    from: { version: "2.0.0", hash: hash("2.0.0") }, to: { version: "1.0.0", hash: hash("1.0.0") },
    changes: [], kept: true, settings: ["greeneye/map/minimap_config.ini"], notices: [],
  };
  expect(rollbackLine(r)).toBe("Its settings from before the update come back with it (1 file). 2.0.0 is kept: roll back again to return to it.");
  expect(rollbackLine({ ...r, settings: [], kept: false, notices: ["The Worst Map 2.0.0 is not kept (Settings keeps no earlier versions)."] }))
    .toBe("The Worst Map 2.0.0 is not kept (Settings keeps no earlier versions).");
  expect(rollbackLine({ ...r, settings: [], kept: false })).toBeUndefined();
});
