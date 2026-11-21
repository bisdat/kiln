// EMF mods whose dependency is missing (kiln_manage::emf_deps; PepegaJedi's Mage Skills report, 0.2.0-dev.30.1): the row
// says what is missing and how to get it before Play, the count line names it, and after a start the row says what
// kiln-emf decided (kiln/state/emf.json) when the preview has nothing to say.
import { expect, test } from "bun:test";
import type { BootState, EmfDepProblem, Json, LibraryEntry, ModKind, Profile, ProfileMod, ResolvePreview } from "../src/api/commands";
import { emptyMods } from "../src/api/kinds";
import { buildRows, countLine, emfDepNote, type ModInputs } from "../src/shell/mods";
import { steps } from "../src/pages/mods/LoadOrder";

const hash = (s: string) => `kdh1:${s.padEnd(64, "0").slice(0, 64)}`;
const lib = (kind: ModKind, id: string, name = id, version = "1.0.0"): LibraryEntry => ({
  kind, id, name, versions: [{ version, hash: hash(id + version), folder: `library/${kind}/${id}/${version}`, imported: "2026-11-01T00:00:00Z", side: "both", clients: "mod",
    size: 1000, source: null, manifestId: null, manifest: null }],
});
const pm = (id: string, enabled = true, version = "1.0.0"): ProfileMod => ({ id, version, hash: hash(id + version), enabled });
const profile = (mods: Partial<Record<ModKind, ProfileMod[]>>): Profile => ({
  format: 1, id: "p", name: "P", created: "", updated: "", generation: 1, notes: "", mods: { ...emptyMods(), ...mods }, dropins: [], dataDir: "shared", server: null,
});
const inputs = (x: Partial<ModInputs>): ModInputs => ({ library: [], profile: null, preview: null, found: [], launch: null, retained: [], xhlHost: null, ...x });
const preview = (emf: EmfDepProblem[]): ResolvePreview => ({ side: "client", kiln: { order: [], rejected: [] }, native: [], warnings: [], withWorld: [], emf });
const missingLua: EmfDepProblem = { id: "mage_skills", dep: "lua", depName: "EMF's lua mod", range: ">=1.36", state: "missing", found: null, withEmf: true,
  url: "https://github.com/matenm/emf-enshrouded", reason: "needs lua, which is not installed" };
const library = [lib("emf", "mage_skills", "Mage Skills"), lib("emf", "beastmaster", "Beastmaster", "1.17.1")];
const p = profile({ emf: [pm("mage_skills"), pm("beastmaster", true, "1.17.1")] });

test("a missing EMF lua mod: the row says it is not installed, that it comes with EMF's release, and links EMF's page", () => {
  const rows = buildRows(inputs({ library, profile: p, preview: preview([missingLua]) }));
  const mage = rows.find((r) => r.id === "mage_skills")!;
  expect(mage.note).toMatchObject({ text: "Needs EMF's lua mod >=1.36, which isn't installed.", tone: "warn",
    hint: "It comes with EMF's release: add EMF's zip with Add mods.",
    link: { label: "Get EMF", url: "https://github.com/matenm/emf-enshrouded", title: "Opens EMF's page, where its release zip is" } });
  expect(mage.note!.title).toBe("Needs EMF's lua mod >=1.36, which isn't installed. It comes with EMF's release: add EMF's zip with Add mods. EMF says: needs lua, which is not installed.");
  // Play's sentence names it, with how to get it; the other EMF mod has nothing to say
  expect(countLine(rows).text).toBe("2 mods on. Mage Skills will not run: it needs EMF's lua mod >=1.36, which isn't installed (it comes with EMF's release: add EMF's zip with Add mods).");
  expect(rows.find((r) => r.id === "beastmaster")!.note).toBeNull();
  // the load order puts it among the mods that do not load, with the row's words
  const s = steps(rows, preview([missingLua]), "client");
  expect(s.notLoading.map((r) => r.id)).toEqual(["mage_skills"]);
});

test("Get EMF opens EMF's page for one of EMF's own mods, whatever link the problem carries", () => {
  const n = emfDepNote({ ...missingLua, url: "https://example.com/not-emf" });
  expect(n.note.link).toMatchObject({ label: "Get EMF", url: "https://github.com/matenm/emf-enshrouded" });
});

test("another mod's missing dependency is named, with the page its mod.json gives, and no EMF hint", () => {
  const n = emfDepNote({ ...missingLua, dep: "pet-core", depName: null, range: "^2.0", withEmf: false, url: "https://www.nexusmods.com/enshrouded/mods/9001", reason: "needs pet-core, which is not installed" });
  expect(n.note).toMatchObject({ text: "Needs pet-core ^2.0, which isn't installed.", hint: undefined,
    link: { label: "Mod page", url: "https://www.nexusmods.com/enshrouded/mods/9001", title: "Opens the page the mod gives for pet-core" } });
  expect(n.why).toBe("it needs pet-core ^2.0, which isn't installed");
  // without a link: named, nothing to open; any version: no range
  const bare = emfDepNote({ ...missingLua, dep: "pet-core", depName: null, range: "", withEmf: false, url: null });
  expect(bare.note.text).toBe("Needs pet-core, which isn't installed.");
  expect(bare.note.link).toBeUndefined();
});

test("off, not loading, too old and too new say so, and only what is missing or too old gets a link", () => {
  const off = emfDepNote({ ...missingLua, state: "off", depName: "Lua Scripts", found: "1.39.0", reason: "needs lua, which is switched off" });
  expect(off.note).toMatchObject({ text: "Needs EMF's lua mod >=1.36, which is switched off.", hint: "Switch it on in this list." });
  expect(off.note.link).toBeUndefined();
  expect(off.why).toBe("it needs EMF's lua mod >=1.36, which is switched off");
  const old = emfDepNote({ ...missingLua, state: "tooOld", found: "1.33.0" });
  expect(old.note).toMatchObject({ text: "Needs EMF's lua mod >=1.36; you have 1.33.0, which is too old.", hint: "A newer one comes with EMF's release: add EMF's zip with Add mods.",
    link: { label: "Get EMF" } });
  const nw = emfDepNote({ ...missingLua, dep: "beastmaster", depName: "Beastmaster", range: "^1.2", state: "tooNew", found: "2.0.0" });
  expect(nw.note).toMatchObject({ text: "Needs Beastmaster ^1.2; you have 2.0.0, which is too new.", hint: "Look for an update of this mod." });
  expect(nw.note.link).toBeUndefined();
  expect(emfDepNote({ ...missingLua, state: "notLoading", found: "1.39.0" }).note.text).toBe("Needs EMF's lua mod >=1.36, which won't load.");
});

test("a switched-off EMF mod says nothing about its dependencies", () => {
  const rows = buildRows(inputs({ library, profile: profile({ emf: [pm("mage_skills", false)] }), preview: preview([missingLua]) }));
  expect(rows.find((r) => r.id === "mage_skills")!.note).toBeNull();
});

// kiln/state/emf.json in the shape of the report's (synthetic values), written during the run boot.json describes
const boot = { pid: 4242, started: "2026-01-02T10:00:00Z", written: "2026-01-02T10:00:09Z" } as BootState["boot"];
const launch = { boot, stale: false, native: null, mods: null } as BootState;
const emfJson = (mods: Json[], at = Date.parse("2026-01-02T10:00:04.500Z")): Json => ({ format: 1, plugin: "kiln-emf", updatedUnixMs: at, mods });
const rejected = (reason: string, version = "1.0.0"): Json => ({ id: "mage_skills", profileId: "mage_skills", origin: "profile", status: "rejected", version, reason });

test("after a start, a rejection kiln-emf wrote is the row's note when the preview has nothing to say", () => {
  const cycle = emfJson([rejected("a cycle of required dependencies")]);
  const rows = buildRows(inputs({ library, profile: p, preview: preview([]), launch, emfState: cycle }));
  expect(rows.find((r) => r.id === "mage_skills")!.note).toMatchObject({ text: "Did not load at the last start", tone: "warn", title: "A cycle of required dependencies." });
  // the report's own case without a preview (not read yet): the rejection is said
  const report = emfJson([rejected("needs lua, which is not installed")]);
  expect(buildRows(inputs({ library, profile: p, launch, emfState: report })).find((r) => r.id === "mage_skills")!.note)
    .toMatchObject({ text: "Did not load at the last start", title: "Needs lua, which is not installed." });
  // with the preview: its words come first (what to get) ...
  expect(buildRows(inputs({ library, profile: p, preview: preview([missingLua]), launch, emfState: report })).find((r) => r.id === "mage_skills")!.note!.text)
    .toBe("Needs EMF's lua mod >=1.36, which isn't installed.");
  // ... and once the player added lua, the old "needs lua" is not said again before the next start
  expect(buildRows(inputs({ library, profile: p, preview: preview([]), launch, emfState: report })).find((r) => r.id === "mage_skills")!.note).toBeNull();
});

test("a rejection from an earlier run, or of another version, is not said", () => {
  const old = emfJson([rejected("a cycle of required dependencies")], Date.parse("2026-01-01T10:00:00Z"));
  expect(buildRows(inputs({ library, profile: p, preview: preview([]), launch, emfState: old })).find((r) => r.id === "mage_skills")!.note).toBeNull();
  const other = emfJson([rejected("a cycle of required dependencies", "0.9.0")]);
  expect(buildRows(inputs({ library, profile: p, preview: preview([]), launch, emfState: other })).find((r) => r.id === "mage_skills")!.note).toBeNull();
  // "1.0" in emf.json is the Library's 1.0.0
  const short = emfJson([rejected("a cycle of required dependencies", "1.0")]);
  expect(buildRows(inputs({ library, profile: p, preview: preview([]), launch, emfState: short })).find((r) => r.id === "mage_skills")!.note).not.toBeNull();
  // no boot.json: nothing tells which run wrote it
  const any = emfJson([rejected("a cycle of required dependencies")]);
  expect(buildRows(inputs({ library, profile: p, preview: preview([]), emfState: any })).find((r) => r.id === "mage_skills")!.note).toBeNull();
});

test("after a start, only what the preview judges again is dropped once it finds nothing; the rest stays", () => {
  const note = (mods: Json[]) => buildRows(inputs({ library, profile: p, preview: preview([]), launch, emfState: emfJson(mods) })).find((r) => r.id === "mage_skills")!.note;
  const lua = (reason: string): Json => ({ id: "lua", origin: "legacy", status: "disabled", version: "1.39.0", reason });
  // judged again by the preview (from the mod.json and the profile): not installed, switched off in the profile, out of range
  expect(note([rejected("needs lua, which is not installed")])).toBeNull();
  expect(note([rejected("needs lua, which is switched off"), lua("switched off in the profile")])).toBeNull();
  expect(note([rejected("needs lua >=1.36 (1.36.0 or newer), found 1.33.0: too old (update lua)")])).toBeNull();
  // not seen by the preview: EMF's ini (or mod.json) keeping lua off, a dependency that did not load, the EMF API version
  expect(note([rejected("needs lua, which is switched off"), lua("switched off (enabled=0)")])).toMatchObject({ text: "Did not load at the last start",
    title: "Needs lua, which is switched off." });
  expect(note([rejected("needs lua, which is not loading (its DLL lua.dll is missing)")])).toMatchObject({ text: "Did not load at the last start" });
  expect(note([rejected("needs EMF API v50; kiln-emf provides v46")])).toMatchObject({ title: "Needs EMF API v50; kiln-emf provides v46." });
});
