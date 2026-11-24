// Mock answers for the Crucible commands (crucible_* and pack_*; browser mock mode, ./mock.ts registers them): a small
// Crucible with a handful of packs, an installed instance with an update waiting (one conflict), the player's own shared
// packs, and the trust list. The URL picks extra cases:
//   ?crucible=off       crucible_status.enabled false: every Crucible surface hides
//   ?crucible=dev       a dev override is in use ("Crucible (dev)")
//   ?crucible=shared    "Ember run" was shared before and changed since (1 added, 1 updated)
//   ?crucible=packs     an instance "Ashen Frontier" (1.0.0, its 1.1.0 waiting with one conflict) is installed
//   ?crucible=instance  the same, and it is the loaded profile (its update banner shows on Play)
//   ?crucible=first     the first start of a Crucible launcher with no pack waiting (as after an update): the clipboard
//                       is not read
//   ?crucible=waiting   the first start after a pack page's Get Kiln whose name lost its code: the clipboard is read once
//   ?crucible=resume    the download carried a pack code (Cinder Trails, K4TW-9XRB): its preview opens at start
// Tests and the console: __kilnCrucibleMock.link(url) (a kiln://crucible link arrives), .setOnline(bool),
// .requests() (every crucible/pack call with its arguments), .state(), .setChecks(versionId, checks) (Crucible's check
// of a version: Cinder Trails passed, Frost Hollow still running, Deep Shroud one warning, shared packs queued).
import type {
  Activation, BuiltPack, CrucibleInstallRequest, CrucibleInstallResult, CruciblePack, CruciblePreview, CrucibleShareRequest, CrucibleShareResult,
  CrucibleChecks, CrucibleShared, CrucibleStatus, CrucibleTrust, CrucibleUpdate, CrucibleUpdatePlan, CrucibleUpdateRequest, CrucibleUpdateResult, CrucibleVersion,
  ImportReport, KilnError, LibraryEntry, LibraryRef, ModKind, PackIndex, PackInstallRequest, PackInstallResult, PackInstance, PackMod, PackPreview,
  PackUpdatePlan, Profile, SaveBackup,
} from "./commands";
import { MOD_KINDS, emptyMods } from "./kinds";

type Handler = (args: Record<string, unknown>) => unknown;
type StartTask = (kind: string, title: string, steps: string[], finish: () => unknown, stepMs?: number) => string;
export interface CrucibleMockDeps {
  handlers: Record<string, Handler>;
  emit: (name: string, payload: unknown) => void;
  startTask: StartTask;
  profiles: Map<string, Profile>;
  library: LibraryEntry[];
  activations: Record<string, Activation | null>;
  activate: (pid: string, gameId: string) => Activation;
  importPaths: (paths: string[]) => ImportReport;
  makeBackup: (gameId: string, reason: SaveBackup["reason"], note: string | null, profileId?: string | null) => SaveBackup;
  slug: (name: string) => string;
  isRunning: (gameId: string) => boolean;
  emitGame: (gameId: string) => void;
  /** The seeded launcher (not ?mock=fresh). */
  seeded: boolean;
}

const q = typeof location !== "undefined" ? new URLSearchParams(location.search) : new URLSearchParams();
const FLAGS = new Set((q.get("crucible") ?? "").split(",").map((s) => s.trim()).filter(Boolean));
const API = "https://frostdev.io/kiln/crucible/api/v1";
const SITE = "https://frostdev.io/kiln/crucible";
const MB = 1_048_576;
const now = () => new Date().toISOString();
const ago = (d: number) => new Date(Date.now() - d * 86_400_000).toISOString();
const clone = <T,>(v: T): T => JSON.parse(JSON.stringify(v));
function err(code: KilnError["code"], message: string, detail?: unknown): never {
  throw { code, message, ...(detail !== undefined ? { detail } : {}) } satisfies KilnError;
}
const hex = (seed: string) => {
  let h = 0x811c9dc5, out = "";
  for (let r = 0; out.length < 64; r++) { for (const c of seed + r) h = Math.imul(h ^ c.charCodeAt(0), 16777619) >>> 0; out += h.toString(16).padStart(8, "0"); }
  return out.slice(0, 64);
};
const kdh = (kind: string, id: string, version: string) => "kdh1:" + hex(kind + id + version);

/** A mod of a mock pack: kind, id, version, name, size in MB, and its credit. */
interface MockMod { kind: ModKind; id: string; version: string; name: string; mb: number; author: string; source: string; optional?: boolean; on?: boolean; code?: boolean }
interface MockVersion { id: string; number: number; version: string; changes: string | null; mods: MockMod[]; status: CrucibleVersion["status"]; progress?: number; published: string }
interface MockPack { pack: CruciblePack; versions: MockVersion[]; kiln?: string; kilnOk?: boolean; builds: number[]; description: string; gone?: boolean }

const ME = { id: "731204485922648124", name: "emberfox" };
const nexus = (n: number) => "https://www.nexusmods.com/enshrouded/mods/" + n;
const m = (kind: ModKind, id: string, version: string, name: string, mb: number, author: string, source: string, extra: Partial<MockMod> = {}): MockMod =>
  ({ kind, id, version, name, mb, author, source, ...extra });

function pack(id: string, slug: string, code: string, title: string, summary: string, owner: { id: string; name: string } | null, upvotes: number, builds: number[], icon: string | null = null): CruciblePack {
  return { id, slug, code, url: SITE + "/p/" + code, title, summary, icon, owner, visibility: "link", upvotes, comments: Math.round(upvotes / 9), installs: upvotes * 7, mods: 0, builds };
}

const packs: MockPack[] = [
  {
    pack: pack("pk_cinder", "cinder-trails", "K4TW-9XRB", "Cinder Trails", "Warm nights, brighter camps and a few new tools.", { id: "402981117", name: "Cinderhawk" }, 128, [26404966]),
    builds: [26404966], description: "A cosy start for a new world.",
    versions: [{ id: "pv_cinder3", number: 3, version: "1.2.0", changes: "Cozy Camp is optional now.", status: "ready", published: ago(1), mods: [
      m("kiln", "lantern-glow", "1.2.0", "Lantern Glow", 18.4, "emberwright", nexus(310)),
      m("kiln", "night-sky", "0.3.1", "Night Sky", 22.0, "stargazer", nexus(313)),
      m("kiln", "weather-plus", "1.0.0", "Weather Plus", 9.6, "cloudkeeper", nexus(316)),
      m("native", "fps-unlock", "1.0.0", "FPS Unlock", 0.4, "framesmith", "https://github.com/framesmith/fps-unlock", { code: true }),
      m("kiln", "cozy-camp", "2.0.0", "Cozy Camp", 31.2, "hearthling", nexus(317), { optional: true, on: true }),
      m("kiln", "hardcore-hunger", "0.3.0", "Hardcore Hunger", 2.1, "cinderhawk", "https://github.com/cinderhawk/hardcore-hunger", { optional: true, on: false }),
    ] }],
  },
  {
    pack: pack("pk_frost", "frost-hollow", "M2PD-7FJC", "Frost Hollow", "Snow, ice and a long winter.", { id: "559201843", name: "Wren" }, 41, [26430253]),
    builds: [26430253], description: "Winter all year.",
    versions: [{ id: "pv_frost1", number: 1, version: "1.0.0", changes: null, status: "uploading", progress: 62, published: ago(0), mods: [
      m("kiln", "deep-winter", "1.0.0", "Deep Winter", 54.0, "wren", "https://github.com/wren/deep-winter"),
      m("kiln", "night-sky", "0.3.1", "Night Sky", 22.0, "stargazer", nexus(313)),
    ] }],
  },
  {
    pack: pack("pk_deep", "deep-shroud", "Q8NV-4KMA", "Deep Shroud", "The shroud never lifts.", { id: "660392011", name: "Vex" }, 12, [26404966]),
    builds: [26404966], kiln: ">=0.2.0-dev.31", kilnOk: false, description: "Needs the next Kiln.",
    versions: [{ id: "pv_deep1", number: 1, version: "0.1.0", changes: null, status: "ready", published: ago(2), mods: [
      m("kiln", "shroud-everywhere", "0.1.0", "Shroud Everywhere", 6.2, "vex", "https://github.com/vex/shroud-everywhere"),
    ] }],
  },
  {
    pack: pack("pk_ashen", "ashen-frontier", "R7MX-2HNQ", "Ashen Frontier", "Hardcore survival, unified loot across the mods.", { id: "318840225", name: "Stonewright" }, 2140, [26404966]),
    builds: [26404966], description: "Hardcore survival.",
    versions: [
      { id: "pv_ashen1", number: 1, version: "1.0.0", changes: null, status: "ready", published: ago(9), mods: [
        m("kiln", "new-items", "1.0.0", "New Items", 12.0, "stonewright", nexus(320)),
        m("kiln", "big-stacks", "1.4.0", "Bigger Stacks", 1.1, "packrat", nexus(312)),
        m("kiln", "kiln-auto-loot", "0.1.7", "Kiln Auto Loot", 0.8, "frostdev", "https://frostdev.io/kiln"),
        m("kiln", "more-hotbars", "2.1.0", "More Hotbars", 0.6, "hotkey", nexus(318)),
      ] },
      { id: "pv_ashen2", number: 2, version: "1.1.0", changes: "Busy Hands joins; loot rebalanced.", status: "ready", published: ago(1), mods: [
        m("kiln", "new-items", "1.0.0", "New Items", 12.0, "stonewright", nexus(320)),
        m("kiln", "kiln-auto-loot", "0.1.8", "Kiln Auto Loot", 0.8, "frostdev", "https://frostdev.io/kiln"),
        m("kiln", "more-hotbars", "2.2.0", "More Hotbars", 0.6, "hotkey", nexus(318)),
        m("kiln", "busy-hands", "0.2.8", "Busy Hands", 1.9, "frostdev", "https://frostdev.io/kiln"),
      ] },
    ],
  },
  {
    pack: pack("pk_gone", "gone-pack", "W3RX-8TDN", "Gone Pack", "", { id: "1", name: "Someone" }, 0, []),
    builds: [], gone: true, description: "", versions: [],
  },
];
for (const p of packs) p.pack.mods = p.versions.at(-1)?.mods.length ?? 0;

/** The player's own shared packs, by profile: the pack, its last version, and what that version held. */
interface Shared { pack: CruciblePack; version: CrucibleVersion; snapshot: string[] }
const shared = new Map<string, Shared>();
const instances = new Map<string, PackInstance>();
const undos = new Map<string, { profile: Profile; instance: PackInstance }>();
/** The player layers (local.<pid>) by profile. */
const localFiles = new Map<string, { path: string; text: string }[]>();
let resumed = false;
const OVERLAP_KEY = '["items","Material_T1_Wood_Logs","maxStackSize"]';
const trust: CrucibleTrust[] = [{ id: "318840225", name: "Stonewright", since: ago(9) }];
const links: string[] = [];
let online = true;
let codeSeq = 0;
const requests: { cmd: string; args: unknown }[] = [];

const latest = (p: MockPack) => p.versions.at(-1)!;
const passed = (): CrucibleChecks => ({ state: "done", full: true, ok: true, errors: [], warnings: [], notes: [], summary: { errors: 0, warnings: 0, notes: 0 } });
/** Crucible's check per version (GET /codes' version.checks); a version not named here: passed. */
const checks = new Map<string, CrucibleChecks>([
  ["pv_frost1", { state: "running", full: false, ok: true, errors: [], warnings: [], notes: [], summary: { errors: 0, warnings: 0, notes: 0 } }],
  ["pv_deep1", { state: "done", full: true, ok: true, errors: [], warnings: ["shroud-everywhere: references networking (System.Net.Http)"], notes: [], summary: { errors: 0, warnings: 1, notes: 0 } }],
]);
const versionOf = (v: MockVersion): CrucibleVersion => ({
  id: v.id, number: v.number, version: v.version, changes: v.changes, size: Math.round(v.mods.reduce((s, x) => s + x.mb, 0) * MB),
  published: v.published, status: v.status, checks: clone(checks.get(v.id) ?? passed()),
});
const indexOf = (p: MockPack, v: MockVersion): PackIndex => ({
  format: 2, kind: "pack", id: p.pack.slug, name: p.pack.title, version: v.version, created: v.published, authors: p.pack.owner ? [p.pack.owner.name] : [],
  description: p.description, game: { builds: p.builds }, kiln: p.kiln ?? ">=0.2.0-dev.30", api: ">=0.2.11",
  crucible: { pack: p.pack.id, version: v.id, slug: p.pack.slug, code: p.pack.code }, changes: v.changes ?? undefined,
  mods: v.mods.map((x): PackMod => ({
    kind: x.kind, id: x.id, version: x.version, name: x.name, path: "mods/" + x.kind + "/" + x.id, hash: kdh(x.kind, x.id, x.version),
    dep: x.optional ? "optional" : "required", ...(x.optional ? { default: !!x.on } : {}),
    file: { sha256: hex("f" + x.id + x.version), size: Math.round(x.mb * MB), url: "https://cdn.frostdev.io/f/" + hex("f" + x.id + x.version) + ".zip" },
    credit: { author: x.author, source: x.source },
  })),
  layer: { id: "pack." + p.pack.slug, version: v.version, path: "layer", hash: kdh("kiln", "pack." + p.pack.slug, v.version) },
  order: { kiln: [...v.mods.filter((x) => x.kind === "kiln").map((x) => x.id), "pack." + p.pack.slug], late: [] },
  overrides: [{ path: "kiln-auto-loot.json", mode: "merge-patch", side: "both", sha256: hex("o" + v.id), size: 120 }],
});

export function crucibleMockHandlers(d: CrucibleMockDeps): Record<string, Handler> {
  const account = (): { id: string; name: string } | null => {
    const s = d.handlers.community_status?.({}) as { account?: { id: string; name: string } | null } | undefined;
    return s?.account ? { id: s.account.id, name: s.account.name } : null;
  };
  const inLibrary = (kind: ModKind, id: string, version: string) => d.library.some((e) => e.kind === kind && e.id === id && e.versions.some((v) => v.version === version));
  const refOf = (x: MockMod): LibraryRef => ({ kind: x.kind, id: x.id, version: x.version, hash: kdh(x.kind, x.id, x.version) });
  const instanceOf = (packId: string) => [...instances.entries()].find(([pid, i]) => i.pack === packId && d.profiles.has(pid))?.[0] ?? null;
  const gameBuild = () => {
    const g = (d.handlers.game_list?.({}) as { kind: string; buildId: string | null }[] | undefined)?.find((x) => x.kind === "client");
    return g?.buildId ? Number(g.buildId) : null;
  };
  const fileName = (x: { kind: ModKind; id: string; version: string }) => (x.kind === "native" ? x.id + "-native-" : x.id + "-") + x.version + ".zip";

  /** `name`, or "name (2)"... when a profile has it (the backend's profile::unique_name). */
  const uniqueMockName = (name: string) => {
    const taken = new Set([...d.profiles.values()].map((p) => p.name.trim().toLowerCase()));
    if (!taken.has(name.toLowerCase())) return name;
    for (let n = 2; ; n++) if (!taken.has((name + " (" + n + ")").toLowerCase())) return name + " (" + n + ")";
  };
  /** The player's own shared packs as Crucible has them. */
  const ownPacks = (): MockPack[] => [...shared.entries()].map(([pid, s]) => {
    const prof = d.profiles.get(pid);
    const mods = prof ? MOD_KINDS.flatMap((k) => (prof.mods[k] ?? []).map((x) => m(k, x.id, x.version, d.library.find((e) => e.kind === k && e.id === x.id)?.name ?? x.id, 4, ME.name, SITE, x.enabled ? {} : { optional: true, on: false }))) : [];
    return { pack: s.pack, builds: s.pack.builds, description: "", versions: [{ id: s.version.id, number: s.version.number, version: s.version.version, changes: s.version.changes, status: "ready" as const, published: s.version.published ?? now(), mods }] };
  });
  /** A code, link, slug or version id -> the pack and the version it names. */
  const find = (target: string): { p: MockPack; v: MockVersion } => {
    let t = target.trim();
    const all = [...packs, ...ownPacks()];
    if (/^pv_[A-Za-z0-9]+$/.test(t)) {
      for (const p of all) { const v = p.versions.find((x) => x.id === t); if (v) return { p, v }; }
      err("not_found", "We couldn't find that pack version.");
    }
    t = t.replace(/^kiln:\/\/crucible\//i, "").replace(/^https:\/\/frostdev\.io\/kiln\/crucible\//i, "").replace(/^frostdev\.io\/kiln\/crucible\//i, "");
    const bySlug = /^(?:pack\/)?([a-z0-9-]{3,})$/.exec(t);
    const code = t.replace(/^p\//i, "").replace(/[\s-]/g, "").toUpperCase();
    const p = /^[0-9A-Z]{8}$/.test(code) ? all.find((x) => x.pack.code.replace("-", "") === code) : undefined;
    const s = !p && bySlug ? all.find((x) => x.pack.slug === bySlug[1]) : undefined;
    const hit = p ?? s;
    if (!hit) err("not_found", "We couldn't find that code. Codes look like H7QK-3MZP.");
    if (hit.gone) err("refused", "This pack is no longer on Crucible.");
    return { p: hit, v: latest(hit) };
  };

  const preview = (p: MockPack, v: MockVersion): CruciblePreview => {
    if (v.status === "uploading") {
      const progress = v.progress ?? 0;
      // the creator's upload goes on while a friend waits
      v.progress = Math.min(100, progress + 19);
      if (progress < 100) err("busy", (p.pack.owner?.name ?? p.pack.title) + "'s pack is still uploading (" + progress + "%). Try again when it's ready.",
        { uploading: { version: v.id, progress: progress / 100 }, pack: clone(p.pack) });
      v.status = "ready";
    }
    const index = indexOf(p, v);
    const missing = v.mods.filter((x) => !inLibrary(x.kind, x.id, x.version)).map(refOf);
    const present = v.mods.filter((x) => inLibrary(x.kind, x.id, x.version)).map(refOf);
    const layer = { kind: "kiln" as ModKind, id: "pack." + p.pack.slug, version: v.version, hash: index.layer!.hash };
    if (!inLibrary("kiln", layer.id, layer.version)) missing.push(layer);
    const cur = gameBuild();
    const r: CruciblePreview = {
      pack: clone(p.pack), version: versionOf(v), index, size: versionOf(v).size,
      download: Math.round(v.mods.filter((x) => !inLibrary(x.kind, x.id, x.version)).reduce((s, x) => s + x.mb, 0) * MB) + 40_960,
      missing, present, code: v.mods.filter((x) => x.code || x.kind === "native" || x.kind === "plugin").map((x) => x.id),
      optional: Object.fromEntries(v.mods.filter((x) => x.optional).map((x) => [x.id, !!x.on])),
      instance: instanceOf(p.pack.slug),
      game: { builds: p.builds, current: cur, fits: !p.builds.length || cur === null || p.builds.includes(cur) },
      kiln: { needs: p.kiln ?? null, ok: p.kilnOk !== false },
      trusted: !!p.pack.owner && trust.some((x) => x.id === p.pack.owner!.id),
    };
    return r;
  };

  /** A new profile holding the mods and the layer, its instance, activated in the game (kiln_manage::modpack::pack::install). */
  const makeInstance = (gameId: string, p: { slug: string; title: string; code?: string; packId?: string; versionId?: string }, version: string, mods: MockMod[],
    optional: Record<string, boolean>, name?: string) => {
    const previous = gameId ? d.activations[gameId]?.profileId ?? null : null;
    const rep = d.importPaths([...mods.map(fileName), "pack." + p.slug + "-" + version + ".zip"]);
    const prof: Profile = { format: 1, id: d.slug(name || p.title), name: name || p.title, created: now(), updated: now(), generation: 1, notes: "",
      mods: emptyMods(), dropins: [], dataDir: "profile", server: null };
    for (const x of mods) prof.mods[x.kind].push({ id: x.id, version: x.version, hash: kdh(x.kind, x.id, x.version), enabled: x.optional ? optional[x.id] ?? !!x.on : true });
    const layer = [...rep.imported, ...rep.existing].find((r) => r.id === "pack." + p.slug);
    if (layer) prof.mods.kiln.push({ id: layer.id, version: layer.version, hash: layer.hash, enabled: true });
    // the Library's entries carry the hashes and names the index gives (the mock's import makes up its own)
    for (const x of mods) {
      const e = d.library.find((l) => l.kind === x.kind && l.id === x.id);
      const ver = e?.versions.find((v) => v.version === x.version);
      if (ver) ver.hash = kdh(x.kind, x.id, x.version);
      if (e && x.name && e.name.toLowerCase() === x.id.replace(/-/g, " ")) e.name = x.name;
    }
    d.profiles.set(prof.id, prof);
    const inst: PackInstance = { format: 1, pack: p.slug, name: p.title, version, sha256: hex("pack" + p.slug + version),
      ...(p.packId ? { crucible: { pack: p.packId, version: p.versionId, slug: p.slug, code: p.code } } : {}), optional, installed: now() };
    instances.set(prof.id, inst);
    if (gameId) { d.makeBackup(gameId, "modpack", null, prof.id); d.activate(prof.id, gameId); d.emitGame(gameId); }
    return { prof, inst, imported: rep.imported, previous };
  };

  const planFor = (pid: string): { p: MockPack; v: MockVersion; plan: PackUpdatePlan } => {
    const inst = instances.get(pid) ?? err("not_found", "That profile is not from a pack.");
    const p = packs.find((x) => x.pack.slug === inst.pack) ?? err("not_found", "This pack is no longer on Crucible.");
    const from = p.versions.find((x) => x.version === inst.version) ?? p.versions[0];
    const v = latest(p);
    const prof = d.profiles.get(pid)!;
    const had = new Map(from.mods.map((x) => [x.id, x]));
    const next = new Map(v.mods.map((x) => [x.id, x]));
    const mine = MOD_KINDS.flatMap((k) => (prof.mods[k] ?? []).map((x) => ({ kind: k, ...x })));
    const plan: PackUpdatePlan = {
      profile: pid, pack: p.pack.slug, from: from.version, to: v.version, changes: v.changes ?? undefined,
      mods: {
        added: v.mods.filter((x) => !had.has(x.id)).map((x) => ({ kind: x.kind, id: x.id, version: x.version, enabled: x.optional ? !!x.on : true, name: x.name })),
        updated: v.mods.filter((x) => had.has(x.id) && had.get(x.id)!.version !== x.version).map((x) => ({ kind: x.kind, id: x.id, from: had.get(x.id)!.version, to: x.version, name: x.name })),
        removed: from.mods.filter((x) => !next.has(x.id)).map((x) => ({ kind: x.kind, id: x.id, version: x.version, enabled: true, name: x.name })),
        yours: mine.filter((x) => !had.has(x.id) && !x.id.startsWith("pack.")).map((x) => ({ kind: x.kind, id: x.id, version: x.version, enabled: x.enabled })),
        keptSwitches: mine.filter((x) => had.has(x.id) && next.has(x.id) && !x.enabled).map((x) => x.id),
      },
      layer: { from: from.version, to: v.version, changed: from.version !== v.version },
      files: from === v ? [] : [{ path: "kiln-auto-loot.json", side: "both", mode: "merge-patch", taken: ["smash", "loot.chests"], kept: ["sound"],
        conflicts: [{ key: "range", base: 15, pack: 12, mine: 20 }] },
        // the pack renamed the mod's hotkey file: the player's hotkey moves along
        { path: "kiln-auto-loot/hotkeys.json", side: "both", mode: "merge-patch", taken: [], kept: ["hotkey"], conflicts: [], renamedFrom: "kiln-auto-loot-keys.json" }],
      conflicts: from === v ? 0 : 1,
      // the player layer: two data edits, one the new pack layer sets too
      local: from === v ? null : { id: "local." + pid, files: 1, fields: 2, overlaps: [{ key: OVERLAP_KEY, section: "items", target: "Material_T1_Wood_Logs",
        field: "maxStackSize", pack: 800, base: 500, mine: 999, file: "content/mine.json", changed: true }] },
    };
    return { p, v, plan };
  };

  const snap = (prof: Profile) => MOD_KINDS.flatMap((k) => (prof.mods[k] ?? []).map((x) => k + "/" + x.id + "@" + x.version + (x.enabled ? "" : "~off"))).sort();
  /** Shares (a new version of) the profile's pack. */
  const share = (req: CrucibleShareRequest): CrucibleShareResult => {
    const prof = d.profiles.get(req.profile) ?? err("not_found", "No profile '" + req.profile + "'.");
    const was = shared.get(prof.id);
    const n = (was?.version.number ?? 0) + 1;
    const code = was?.pack.code ?? (prof.id === "ember-run" ? "H7QK-3MZP" : ["T6WB-4NHJ", "C9KF-5RXD", "P3VM-8QGA"][codeSeq++ % 3]);
    const mods = MOD_KINDS.reduce((s, k) => s + (prof.mods[k] ?? []).length, 0);
    const p: CruciblePack = was?.pack ?? { id: "pk_" + prof.id, slug: prof.id, code, url: SITE + "/p/" + code, title: req.title ?? prof.name, summary: "",
      icon: null, owner: account() || FLAGS.has("accountshare") ? { ...ME } : { id: "", name: "Anonymous" }, visibility: "link", upvotes: 0, comments: 0, installs: 0, mods, builds: [26404966] };
    p.mods = mods;
    const size = Math.round((12 + mods * 6) * MB);
    const ver: CrucibleVersion = { id: "pv_" + prof.id.replace(/-/g, "") + n, number: n, version: n + ".0.0", changes: was ? "Updated by the launcher." : null, size, published: now(), status: "ready" };
    // the full check runs a few minutes after an upload
    checks.set(ver.id, { state: "queued", full: false, ok: true, errors: [], warnings: [], notes: [], summary: { errors: 0, warnings: 0, notes: 0 } });
    ver.checks = clone(checks.get(ver.id)!);
    shared.set(prof.id, { pack: p, version: ver, snapshot: snap(prof) });
    return { pack: clone(p), version: ver, files: mods + 1, uploaded: was ? Math.round(size / 9) : Math.round(size * 0.7), skipped: was ? size - Math.round(size / 9) : Math.round(size * 0.3), copied: false };
  };
  const changedOf = (pid: string): CrucibleShared["changed"] => {
    const s = shared.get(pid)!;
    const prof = d.profiles.get(pid);
    if (!prof) return null;
    const key = (x: string) => x.split("@")[0];
    const was = new Map(s.snapshot.map((x) => [key(x), x]));
    const is = new Map(snap(prof).map((x) => [key(x), x]));
    const name = (k: string) => k.split("/")[1];
    const added = [...is.keys()].filter((k) => !was.has(k)).map(name);
    const removed = [...was.keys()].filter((k) => !is.has(k)).map(name);
    const updated = [...is.keys()].filter((k) => was.has(k) && was.get(k) !== is.get(k)).map(name);
    return added.length || removed.length || updated.length ? { added, updated, removed } : null;
  };

  /** A dropped .kilnpack: "cinder-trails-1.2.0.kilnpack" is Cinder Trails, anything else a small pack of its name. */
  const fromFile = (path: string): MockPack => {
    const name = (path.split(/[\\/]/).pop() ?? path).replace(/\.kilnpack$/i, "");
    const slug = name.replace(/-\d+\.\d+\.\d+$/, "").toLowerCase();
    const hit = packs.find((x) => x.pack.slug === slug);
    if (hit) return hit;
    const title = slug.replace(/-/g, " ").replace(/\b\w/g, (c) => c.toUpperCase()) || "Pack";
    return { pack: pack("file_" + slug, slug, "", title, "", null, 0, []), builds: [], description: "", versions: [{ id: "file", number: 1, version: "1.0.0", changes: null, status: "ready", published: now(), mods: [
      m("kiln", "lantern-glow", "1.2.0", "Lantern Glow", 18.4, "emberwright", nexus(310)),
      m("kiln", "weather-plus", "1.0.0", "Weather Plus", 9.6, "cloudkeeper", nexus(316)),
    ] }] };
  };

  // seeds (only with a flag: other tests count the Library's entries): the instance with an update waiting
  // (?crucible=packs, or =instance to load it), and (?crucible=shared) Ember run shared before
  if (d.seeded && (FLAGS.has("packs") || FLAGS.has("instance"))) {
    const ashen = packs.find((x) => x.pack.slug === "ashen-frontier")!;
    const v1 = ashen.versions[0];
    const before = d.activations["client-steam"]?.profileId ?? null;
    const { prof } = makeInstance("", { slug: ashen.pack.slug, title: ashen.pack.title, code: ashen.pack.code, packId: ashen.pack.id, versionId: v1.id }, v1.version, v1.mods, {});
    // the player's own touch: a mod they added (and, in the plan, "range" 20 in kiln-auto-loot.json)
    const sky = d.library.find((l) => l.kind === "kiln" && l.id === "night-sky")?.versions[0];
    if (sky) prof.mods.kiln.push({ id: "night-sky", version: sky.version, hash: sky.hash, enabled: true });
    instances.get(prof.id)!.installed = ago(8);
    prof.created = prof.updated = ago(8);
    if (FLAGS.has("instance")) d.activate(prof.id, "client-steam");
    else if (before) d.activate(before, "client-steam");
  }
  if (d.seeded) {
    if (FLAGS.has("shared") && d.profiles.has("ember-run")) {
      share({ profile: "ember-run" });
      const s = shared.get("ember-run")!;
      s.snapshot = s.snapshot.filter((x) => !x.startsWith("kiln/weather-plus@")).map((x) => (x.startsWith("kiln/night-sky@") ? "kiln/night-sky@0.2.0" : x));
      s.version.published = ago(3);
    }
  }

  const h: Record<string, Handler> = {
    crucible_status: (): CrucibleStatus => ({ enabled: !FLAGS.has("off"), dev: FLAGS.has("dev"), api: FLAGS.has("dev") ? "http://127.0.0.1:8790/api/v1" : API, signedIn: !!account(), account: account() }),
    crucible_open: (a) => {
      if (!online) err("network", "Kiln can't reach Crucible.");
      const { p, v } = find(String(a.target ?? ""));
      return preview(p, v);
    },
    crucible_install: (a) => {
      const gameId = String(a.gameId);
      const req = a.request as CrucibleInstallRequest;
      if (!req?.consent) err("consent_required", "Tick \"I trust this pack\" first.");
      const { p, v } = find(req.target);
      if (p.kilnOk === false) err("refused", "Needs Kiln " + (p.kiln ?? "") + " or newer.");
      if (v.status === "uploading") err("refused", "The pack is still uploading. Try again in a minute.");
      // Install alone (play false) writes only the data folder: it may run while the game does
      const play = req.play !== false;
      if (play && d.isRunning(gameId)) err("game_running", "Close Enshrouded first.");
      const missing = v.mods.filter((x) => !inLibrary(x.kind, x.id, x.version));
      const total = missing.reduce((s, x) => s + x.mb, 0);
      return d.startTask("crucible_install", "Install " + p.pack.title, [
        "Downloading " + missing.length + " mods (" + total.toFixed(0) + " MB)", "Checking the files", "Adding to your Library", "Making the profile", ...(play ? ["Backing up your saves"] : []),
      ], (): CrucibleInstallResult => {
        if (req.trustAlways && p.pack.owner && !trust.some((x) => x.id === p.pack.owner!.id)) trust.push({ id: p.pack.owner.id, name: p.pack.owner.name, since: now() });
        const opt = { ...Object.fromEntries(v.mods.filter((x) => x.optional).map((x) => [x.id, !!x.on])), ...(req.optional ?? {}) };
        const previous = d.activations[gameId]?.profileId ?? null;
        // named after the pack and its creator by default, never a name a profile has (kiln_manage::profile::unique_name)
        const r = makeInstance(play ? gameId : "", { slug: p.pack.slug, title: p.pack.title, code: p.pack.code, packId: p.pack.id, versionId: v.id }, v.version, v.mods, opt,
          uniqueMockName(req.name?.trim() || (p.pack.owner ? p.pack.title + " (" + p.pack.owner.name + "'s pack)" : p.pack.title)));
        p.pack.installs++;
        return { profile: clone(r.prof), instance: clone(r.inst), imported: r.imported, overrides: ["kiln-auto-loot.json"], previous: play ? r.previous : previous, downloaded: Math.round(total * MB) };
      }, 350);
    },
    crucible_updates: (): CrucibleUpdate[] => [...instances.entries()].filter(([pid]) => d.profiles.has(pid)).flatMap(([pid, i]) => {
      const p = packs.find((x) => x.pack.slug === i.pack);
      if (!p) return [];
      const v = latest(p);
      if (v.version === i.version) return [];
      const from = p.versions.find((x) => x.version === i.version);
      const { plan } = planFor(pid);
      return [{ profile: pid, pack: p.pack.slug, name: p.pack.title, from: { version: i.version, number: from?.number ?? null }, to: { versionId: v.id, version: v.version, number: v.number },
        by: p.pack.owner?.name ?? null, counts: { added: plan.mods.added.length, updated: plan.mods.updated.length, removed: plan.mods.removed.length } }];
    }),
    crucible_update_plan: (a): CrucibleUpdatePlan => {
      const { v, plan } = planFor(String(a.profileId));
      return { versionId: v.id, download: Math.round(v.mods.filter((x) => !inLibrary(x.kind, x.id, x.version)).reduce((s, x) => s + x.mb, 0) * MB), plan };
    },
    crucible_update: (a) => {
      const gameId = String(a.gameId);
      const req = a.request as CrucibleUpdateRequest;
      if (!req?.consent) err("consent_required", "Confirm the update first.");
      if (d.isRunning(gameId) && d.activations[gameId]?.profileId === req.profile) err("game_running", "Close Enshrouded first.");
      const { p, v, plan } = planFor(req.profile);
      return d.startTask("crucible_update", "Update " + p.pack.title, ["Downloading what changed", "Merging your settings", "Updating the profile"], (): CrucibleUpdateResult => {
        const open = plan.files.flatMap((f) => f.conflicts.map((c) => f.path + "#" + c.key)).filter((k) => !req.resolve?.[k] && !req.all);
        if (open.length) err("conflict", open.length + " setting(s) need your choice.", plan);
        const known = new Set(plan.local?.overlaps.map((o) => o.key) ?? []);
        for (const k of req.drop ?? []) if (!known.has(k)) err("invalid_argument", k + " is no field both the pack and your changes set");
        const pid = req.profile;
        const prof = clone(d.profiles.get(pid)!);
        const inst = clone(instances.get(pid)!);
        undos.set(pid, { profile: clone(prof), instance: clone(inst) });
        const rep = d.importPaths([...v.mods.map(fileName), "pack." + p.pack.slug + "-" + v.version + ".zip"]);
        const gone = new Set(plan.mods.removed.map((x) => x.id));
        for (const k of MOD_KINDS) prof.mods[k] = (prof.mods[k] ?? []).filter((x) => !gone.has(x.id) && !x.id.startsWith("pack."));
        for (const x of v.mods) {
          const list = prof.mods[x.kind];
          const i = list.findIndex((y) => y.id === x.id);
          const e = { id: x.id, version: x.version, hash: kdh(x.kind, x.id, x.version), enabled: i >= 0 ? list[i].enabled : x.optional ? req.optional?.[x.id] ?? !!x.on : true };
          if (i >= 0) list[i] = e; else list.push(e);
          const ver = d.library.find((l) => l.kind === x.kind && l.id === x.id)?.versions.find((y) => y.version === x.version);
          if (ver) ver.hash = e.hash;
        }
        const layer = [...rep.imported, ...rep.existing].find((r) => r.id === "pack." + p.pack.slug);
        if (layer) prof.mods.kiln.push({ id: layer.id, version: layer.version, hash: layer.hash, enabled: true });
        prof.generation++;
        prof.updated = now();
        d.profiles.set(pid, prof);
        const next: PackInstance = { ...inst, version: v.version, sha256: hex("pack" + p.pack.slug + v.version), updated: now(), previous: inst.version,
          crucible: { ...inst.crucible, version: v.id } };
        instances.set(pid, next);
        for (const [g, act] of Object.entries(d.activations)) if (act?.profileId === pid && !d.isRunning(g)) { d.activate(pid, g); d.emitGame(g); }
        const backups = Object.entries(d.activations).filter(([, act]) => act?.profileId === pid).map(([g]) => d.makeBackup(g, "modpack", "before updating pack " + p.pack.title, pid));
        return { plan, profile: clone(prof), instance: clone(next), imported: rep.imported, backups };
      }, 300);
    },
    crucible_share: (a) => {
      const req = a.request as CrucibleShareRequest;
      if (!account() && shared.get(req.profile)?.pack.owner?.id) err("auth_failed", "Sign in with Discord to update this account-owned share.");
      if (!online) err("network", "Kiln can't reach Crucible.");
      const prof = d.profiles.get(req.profile) ?? err("not_found", "No profile '" + req.profile + "'.");
      const again = shared.has(prof.id);
      const mods = MOD_KINDS.reduce((s, k) => s + (prof.mods[k] ?? []).length, 0);
      const total = again ? Math.max(2, Math.round(mods * 0.7)) : 12 + mods * 6;
      return d.startTask("crucible_share", "Share " + prof.name, [0, 0.25, 0.5, 0.75].map((f) => "Uploading " + Math.round(total * f) + " of " + total + " MB"), () => share(req), 400);
    },
    crucible_shared: (a): CrucibleShared | null => {
      const pid = String(a.profileId);
      const s = shared.get(pid);
      if (!s) return null;
      return { profile: pid, pack: clone(s.pack), version: clone(s.version), changed: changedOf(pid), requiresSignIn: !!s.pack.owner?.id };
    },
    crucible_trust: () => clone(trust),
    crucible_trust_set: (a) => {
      const id = String(a.id);
      const i = trust.findIndex((x) => x.id === id);
      if (a.trusted && i < 0) trust.push({ id, name: String(a.name), since: now() });
      if (!a.trusted && i >= 0) trust.splice(i, 1);
      return null;
    },
    crucible_links: () => links.splice(0),
    // ?crucible=first: a first start with no pack waiting; ?crucible=waiting: a Get Kiln download without its code (the
    // clipboard is read once); ?crucible=resume: the download carried Cinder Trails' code
    crucible_resume: () => {
      const first = (FLAGS.has("first") || FLAGS.has("waiting")) && !resumed;
      const r = { firstStart: first, code: FLAGS.has("resume") && !resumed ? "K4TW9XRB" : null, packWaiting: FLAGS.has("waiting") && !resumed };
      resumed = true;
      return r;
    },
    // the backend reads the OS clipboard; in a browser the page's own (Playwright grants it), raw: the UI decides
    crucible_clipboard: async () => { try { return (await navigator.clipboard?.readText?.()) || null; } catch { return null; } },
    pack_local_files: (a) => clone(localFiles.get(String(a.profileId)) ?? []),
    pack_local_write: (a) => {
      const pid = String(a.profileId), path = String(a.path);
      if (!/^content\/[^/\\.][^/\\]*\.json$/i.test(path)) err("invalid_argument", path + ": the player layer holds content/<name>.json files only");
      const list = (localFiles.get(pid) ?? []).filter((f) => f.path !== path);
      if (a.text != null) list.push({ path, text: String(a.text) });
      localFiles.set(pid, list.sort((x, y) => x.path.localeCompare(y.path)));
      return null;
    },
    pack_local_save: (a) => {
      const pid = String(a.profileId);
      const prof = clone(d.profiles.get(pid) ?? err("not_found", "No profile '" + pid + "'."));
      prof.mods.kiln = prof.mods.kiln.filter((x) => x.id !== "local." + pid);
      if ((localFiles.get(pid) ?? []).length) prof.mods.kiln.push({ id: "local." + pid, version: "1.0.0", hash: hex("local" + JSON.stringify(localFiles.get(pid))), enabled: true });
      prof.generation++;
      d.profiles.set(pid, prof);
      return clone(prof);
    },
    pack_instance: (a) => clone(instances.get(String(a.profileId)) ?? null),
    pack_instances: () => [...instances.entries()].filter(([pid]) => d.profiles.has(pid)).map(([pid, i]) => [pid, clone(i)]),
    pack_undo: (a) => {
      const pid = String(a.profileId);
      const u = undos.get(pid) ?? err("not_found", "There is no update to undo.");
      if (Object.entries(d.activations).some(([g, act]) => act?.profileId === pid && d.isRunning(g))) err("game_running", "Close Enshrouded first.");
      undos.delete(pid);
      d.profiles.set(pid, { ...u.profile, generation: (d.profiles.get(pid)?.generation ?? 1) + 1, updated: now() });
      instances.set(pid, u.instance);
      for (const [g, act] of Object.entries(d.activations)) if (act?.profileId === pid) { d.activate(pid, g); d.emitGame(g); }
      return clone(d.profiles.get(pid)!);
    },
    pack_uninstall: (a) => {
      const pid = String(a.profileId);
      const prof = d.profiles.get(pid) ?? err("not_found", "No profile '" + pid + "'.");
      if (Object.values(d.activations).some((act) => act?.profileId === pid)) err("conflict", "Load another profile first: this one is loaded in the game.");
      d.profiles.delete(pid);
      instances.delete(pid);
      undos.delete(pid);
      const used = new Set([...d.profiles.values()].flatMap((p) => MOD_KINDS.flatMap((k) => (p.mods[k] ?? []).map((x) => k + "/" + x.id + "@" + x.version))));
      const unused = MOD_KINDS.flatMap((k) => (prof.mods[k] ?? []).filter((x) => !used.has(k + "/" + x.id + "@" + x.version)).map((x) => ({ kind: k, id: x.id, version: x.version, hash: x.hash })));
      return { profile: clone(prof), unused };
    },
    // the pack analysis of a file: a path with "unsafe" in it fails, "warn" has a warning
    pack_check: (a) => {
      const p = String(a.path);
      const findings = /unsafe/i.test(p) ? [{ level: "error", code: "zip", message: "entry \"../evil.dll\" has an unsafe path" }]
        : /warn/i.test(p) ? [{ level: "warning", code: "code-network", message: "references networking (System.Net.Http)", mod: "fps-unlock" }] : [];
      const errors = findings.filter((f) => f.level === "error").length;
      return { format: 1, kind: "kiln.pack-check", ok: errors === 0, complete: true, summary: { errors, warnings: findings.length - errors, notes: 0 }, findings, mods: [], conflicts: [], credits: [], catalog: false };
    },
    pack_peek_format: (a) => (/modpack/i.test(String(a.path)) && !/\.kilnpack$/i.test(String(a.path)) ? 1 : 2),
    pack_build: (a) => {
      const req = a.request as { profile: string; version: string; name?: string };
      const prof = d.profiles.get(req.profile) ?? err("not_found", "No profile '" + req.profile + "'.");
      const out = String(a.outDir);
      return d.startTask("pack_build", "Save " + prof.name + " as a file", ["Reading the profile", "Packing the mods", "Checking the file"], (): BuiltPack => {
        const size = Math.round((12 + MOD_KINDS.reduce((s, k) => s + (prof.mods[k] ?? []).length, 0) * 6) * MB);
        return { file: out.replace(/[\\/]+$/, "") + "/" + prof.id + "-" + req.version + ".kilnpack", sha256: hex("built" + prof.id), size,
          index: { format: 2, kind: "pack", id: prof.id, name: req.name ?? prof.name, version: req.version, mods: [], order: { kiln: [], late: [] } }, notes: [] };
      }, 250);
    },
    pack_inspect: (a): PackPreview => {
      const path = String((a.source as { path?: string })?.path ?? "");
      const p = fromFile(path);
      const pv = preview(p, latest(p));
      return { index: { ...pv.index, crucible: undefined }, sha256: hex("file" + path), size: pv.size, missing: pv.missing, present: pv.present, code: pv.code, optional: pv.optional, instance: pv.instance };
    },
    pack_install: (a) => {
      const gameId = String(a.gameId);
      const req = a.request as PackInstallRequest;
      if (!req?.consent) err("consent_required", "Tick \"I trust this pack\" first.");
      if (req.activate !== false && d.isRunning(gameId)) err("game_running", "Close Enshrouded first.");
      const p = fromFile(req.source.path);
      const v = latest(p);
      return d.startTask("pack_install", "Install " + p.pack.title, ["Checking the file", "Adding to your Library", "Making the profile", "Backing up your saves"], (): PackInstallResult => {
        const opt = { ...Object.fromEntries(v.mods.filter((x) => x.optional).map((x) => [x.id, !!x.on])), ...(req.optional ?? {}) };
        const previous = d.activations[gameId]?.profileId ?? null;
        const r = makeInstance(req.activate === false ? "" : gameId, { slug: p.pack.slug, title: p.pack.title }, v.version, v.mods, opt, uniqueMockName(req.name?.trim() || p.pack.title));
        lastPrevious = previous;
        return { profile: clone(r.prof), instance: clone(r.inst), imported: r.imported, overrides: [], backup: null, activation: req.activate === false ? null : d.activations[gameId] ?? null };
      }, 300);
    },
  };
  let lastPrevious: string | null = null;

  if (typeof window !== "undefined") {
    (window as unknown as { __kilnCrucibleMock?: unknown }).__kilnCrucibleMock = {
      /** A kiln://crucible link, as the OS hands it to the launcher. */
      link: (url: string) => { links.push(url); d.emit("kiln://crucible-link", { count: links.length }); },
      setOnline: (on: boolean) => { online = on; },
      setChecks: (versionId: string, c: CrucibleChecks) => { checks.set(versionId, clone(c)); },
      requests: () => clone(requests),
      state: () => clone({ shared: [...shared.entries()], instances: [...instances.entries()], trust, undo: [...undos.keys()], lastPrevious }),
    };
  }
  return Object.fromEntries(Object.entries(h).map(([name, fn]) => [name, (args: Record<string, unknown>) => { requests.push({ cmd: name, args: clone(args) }); return fn(args); }]));
}
