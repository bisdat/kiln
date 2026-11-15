// Kept versions (an earlier version an update kept for a roll back) are never what the launcher picks by itself:
// the row's version, "Changed on disk", the switch, "All on" and the server's sync dialog take the Library's newest
// version that is not kept (src/shell/mods.ts defaultVersion; the review fix of 2026-11-14, kiln_manage::kept).
import { expect, test } from "bun:test";
import type { KeptInfo, LibraryEntry, LibraryVersion, ModKind, Profile, ProfileMod } from "../src/api/commands";
import { emptyMods } from "../src/api/kinds";
import { buildRows, bulkPlan, defaultVersion, type ModInputs } from "../src/shell/mods";

const hash = (s: string) => `kdh1:${s.padEnd(64, "0").slice(0, 64)}`;
const kept: KeptInfo = { kept: "2026-11-14T00:00:00Z", replacedBy: "1.0.0", settings: [], settingsSize: 0 };
const ver = (v: string, k: KeptInfo | null = null, changed = false): LibraryVersion => ({
  version: v, hash: hash(v), folder: `library/greeneye/map/${v}`, imported: "2026-11-01T00:00:00Z", side: "client", clients: "none",
  size: 1000, source: null, manifestId: null, manifest: null, kept: k, changed,
});
const lib = (versions: LibraryVersion[], kind: ModKind = "greeneye"): LibraryEntry => ({ kind, id: "map", name: "Map", versions });
const profile = (mods: Partial<Record<ModKind, ProfileMod[]>>): Profile => ({
  format: 1, id: "p", name: "P", created: "", updated: "", generation: 1, notes: "", mods: { ...emptyMods(), ...mods }, dropins: [], dataDir: "shared", server: null,
});
const inputs = (x: Partial<ModInputs>): ModInputs => ({ library: [], profile: null, preview: null, found: [], launch: null, retained: [], xhlHost: null, ...x });

// After a roll back the newer version is the kept one; the Library lists kept versions last, but the launcher must not
// rely on the order alone (a Library from an older backend, a list with only kept versions left).
const rolledBack = () => lib([ver("2.0.0", kept), ver("1.0.0")]);

test("the default version is the newest that is not kept, and none when only kept ones are left", () => {
  expect(defaultVersion(rolledBack())?.version).toBe("1.0.0");
  expect(defaultVersion(lib([ver("1.0.0"), ver("0.9.0", kept)]))?.version).toBe("1.0.0");
  expect(defaultVersion(lib([ver("1.0.0", kept)]))).toBeUndefined();
  expect(defaultVersion(null)).toBeUndefined();
});

test("a mod the profile lacks shows its default version, not a kept one", () => {
  const [row] = buildRows(inputs({ library: [rolledBack()], profile: profile({}) }));
  expect(row.version).toBe("1.0.0");
  // in the profile: the profile's version, kept or not
  const [inProfile] = buildRows(inputs({ library: [rolledBack()], profile: profile({ greeneye: [{ id: "map", version: "2.0.0", hash: hash("2.0.0"), enabled: false }] }) }));
  expect(inProfile.version).toBe("2.0.0");
});

test("Changed on disk looks at the version the row uses, never a kept one", () => {
  const [row] = buildRows(inputs({ library: [lib([ver("2.0.0", kept, true), ver("1.0.0")])], profile: profile({}) }));
  expect(row.changed).toBeNull();
  const [other] = buildRows(inputs({ library: [lib([ver("2.0.0", kept), ver("1.0.0", null, true)])], profile: profile({}) }));
  expect(other.changed?.version).toBe("1.0.0");
});

test("All on adds the default version; with only kept versions it says to pick one", () => {
  const plan = bulkPlan(buildRows(inputs({ library: [rolledBack()], profile: profile({}) })), true);
  expect(plan.change.map((c) => c.after.version)).toEqual(["1.0.0"]);
  const none = bulkPlan(buildRows(inputs({ library: [lib([ver("2.0.0", kept)])], profile: profile({}) })), true);
  expect(none.change).toEqual([]);
  expect(none.left[0].why).toContain("only kept versions");
});
