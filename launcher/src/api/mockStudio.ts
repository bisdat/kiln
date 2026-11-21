// Mock answers for Pack Studio (studio_*; browser mock mode, ./mock.ts registers them). The seeded launcher has one
// pack source, "Ember run" (from the profile of that name), with a layer file, two settings keys chosen and a credit
// missing. The URL picks extra cases:
//   ?studio=empty       no pack sources yet
//   ?studio=published   "Ember run" 1.0.0 is published (public, credits complete); the next version is 1.0.1
//   ?studio=nocatalog   no game-data catalog on this PC: the pickers list only what the pack's mods add, no fields; Kiln's
//                       own build of it is refused once (Steam updating), and "Build the game catalog" then makes it
//   ?studio=notready    Crucible does not take a pack public yet (its checks still run): publish answers needs, Make public
//   ?studio=gone        the published pack was deleted on Crucible: publish refuses until "Publish as a new pack"
// Tests and the console: __kilnStudioMock.requests() (every studio call with its arguments), .state().
import type {
  BuiltPack, CatalogFieldInfo, StudioItemCategory, CruciblePack, CrucibleVersion, Json, LibraryEntry, LintDiagnostic, LintReport, ModKind, PackCheckReport, PackCredit,
  PackEnv, PackSourceDoc, Profile, Studio, StudioCaptureChoice, StudioCaptureFile, StudioLayerFile, StudioMod, StudioNewPack, StudioOrder, StudioPickHit,
  StudioCatalogStatus, StudioPublishRequest, StudioPublishResult, StudioSummary, StudioTestReport,
} from "./commands";
import { MOD_KINDS, emptyMods } from "./kinds";
import { STATION_WATER, isHiddenGameName } from "./hiddenNames";
/** Hidden in the pickers: an old, test or unused name the game's own data does not use (catalog::used_names; here: the water). */
const hiddenEntry = (name: string) => isHiddenGameName(name) && name !== STATION_WATER;

type Handler = (args: Record<string, unknown>) => unknown;
type StartTask = (kind: string, title: string, steps: string[], finish: () => unknown, stepMs?: number) => string;
export interface StudioMockDeps {
  handlers: Record<string, Handler>;
  startTask: StartTask;
  profiles: Map<string, Profile>;
  library: LibraryEntry[];
  slug: (name: string) => string;
  activate: (pid: string, gameId: string) => unknown;
  seeded: boolean;
}

const q = typeof location !== "undefined" ? new URLSearchParams(location.search) : new URLSearchParams();
const FLAGS = new Set((q.get("studio") ?? "").split(",").map((s) => s.trim()).filter(Boolean));
const now = () => new Date().toISOString();
/** The game-data catalog is on this PC (studio_catalog_ensure makes it in ?studio=nocatalog after one refusal). */
let catalogOk = !FLAGS.has("nocatalog");
let catalogAttempts = 0;
let catalog: StudioCatalogStatus = { state: "idle", exists: catalogOk };
const CATALOG_PATH = "/home/player/.local/share/kiln/catalog/client.json";
const clone = <T,>(v: T): T => JSON.parse(JSON.stringify(v));
function err(code: string, message: string, detail?: unknown): never {
  throw { code, message, ...(detail !== undefined ? { detail } : {}) };
}
const S = (v: unknown) => String(v ?? "");
const hashOf = (text: string) => { let h = 0x811c9dc5; for (let i = 0; i < text.length; i++) h = Math.imul(h ^ text.charCodeAt(i), 16777619) >>> 0; return h.toString(16); };

interface MockStudio { source: PackSourceDoc; credits: Record<string, PackCredit>; layer: Map<string, string> }

/** Game data for the pickers (a few of each section the editors use; displayName as the catalog has it). */
export const GAME: Record<string, { name: string; displayName?: string; id?: string }[]> = {
  items: [
    { name: "Material_T1_Wood_Logs", displayName: "Wood Logs", id: "0x0000a1b2" },
    { name: "Material_T1_Twigs", displayName: "Twigs", id: "0x0000a1b3" },
    { name: "Material_T1_Stone", displayName: "Stone", id: "0x0000a1b4" },
    { name: "Material_T2_Wood_Hardwood", displayName: "Hardwood", id: "0x0000a1b5" },
    { name: "Material_T4_Wood_Logs_Palm", displayName: "Palm Wood Logs", id: "0x0000a1b6" },
    { name: "Material_T5_Wood_Logs_Pine", displayName: "Conifer Logs", id: "0x0000a1b7" },
    { name: "Material_T6_Wood_Logs_Bamboo", displayName: "Bamboo Logs", id: "0x0000a1b8" },
    { name: "Material_T3_Limestone", displayName: "Limestone", id: "0x0000a1b9" },
    { name: "Material_T2_Shroud_Spores", displayName: "Shroud Spores", id: "0x0000a1c0" },
    { name: "Material_T2_Metal_Scraps", displayName: "Metal Scraps", id: "0x0000a1c1" },
    { name: "Material_T3_Iron_Bar", displayName: "Iron Bar", id: "0x0000a1d0" },
    { name: "Weapon_T1_1H_Sword_Craft", displayName: "Crafted Sword", id: "0x0000b001" },
    { name: "Weapon_T2_2H_Axe_Craft", displayName: "Woodcutter's Axe", id: "0x0000b002" },
    { name: "Armor_T2_Chest_Ranger", displayName: "Ranger Jerkin", id: "0x0000b101" },
    { name: "Consumable_Food_Berry_Pie", displayName: "Berry Pie", id: "0x0000c001" },
    { name: "Consumable_Potion_Health_T1", displayName: "Small Health Potion", id: "0x0000c101" },
    // the game's old and test entries (catalog::is_hidden_name): the pickers hide them unless "Show hidden game entries"
    { name: "Z_Material_Sand_DEPRECATED", displayName: "Sand", id: "0x0000a1e0" },
    { name: "Material_T1_Stone_TEST", displayName: "Stone", id: "0x0000a1e1" },
    // a deprecated-looking name the game still uses (station tank water): always listed
    { name: STATION_WATER, displayName: "Water", id: "0xf262a2a9" },
  ],
  recipes: [
    { name: "Recipe_Weapon_T1_1H_Sword_Craft", displayName: "Crafted Sword" },
    { name: "Recipe_Material_T3_Iron_Bar", displayName: "Iron Bar" },
    { name: "Recipe_Consumable_Food_Berry_Pie", displayName: "Berry Pie" },
  ],
  // random loot is per item (content.md "loot")
  loot: [
    { name: "Material_T1_Wood_Logs", displayName: "Wood Logs", id: "0x0000a1b2" },
    { name: "Material_T1_Stone", displayName: "Stone", id: "0x0000a1b4" },
    { name: "Material_T2_Shroud_Spores", displayName: "Shroud Spores", id: "0x0000a1c0" },
  ],
  // drop tables are keyed by their GUID (content.md "drops")
  drops: [
    { name: "Drop_Enemy_Scavenger", displayName: "Scavenger", id: "a20e8eae-7833-462e-9ea9-59d1e17a1b98" },
    { name: "Drop_Enemy_Wolf", displayName: "Wolf", id: "5b0c2f1e-1d3a-4c8e-9f60-2b7d4e0a9c11" },
  ],
  // crafting stations (the catalog's "workshops"; a new recipe's station is "0x<workshop id>" or "hand")
  workshops: [
    { name: "Workshop_Blacksmith", displayName: "Blacksmith", id: "0x0c0c0001" },
    { name: "Workshop_Alchemist", displayName: "Alchemist", id: "0x0c0c0002" },
    { name: "Workshop_Hunter", displayName: "Hunter", id: "0x0c0c0003" },
    { name: "Workshop_Workbench", displayName: "Workbench", id: "0x0c0c0004" },
  ],
};

/** The game's recipe input categories (studio_categories: catalog itemCategories with their items' display names and the
 *  recipes that take them; no names, as the game's). Members count as used by the game: never hidden. */
export const CATEGORIES: StudioItemCategory[] = [
  { id: "0xee5ae93f", items: ["Material_T1_Wood_Logs", "Material_T2_Wood_Hardwood", "Material_T4_Wood_Logs_Palm", "Material_T5_Wood_Logs_Pine", "Material_T6_Wood_Logs_Bamboo"].map(itemOf),
    recipes: [{ name: "Recipe_Tool_Camera", displayName: "Camera" }, { name: "Recipe_Material_Charcoal", displayName: "Charcoal" }, { name: "Recipe_Building_Wood_Block", displayName: "Wood Block" }] },
  { id: "0xb053f0f1", items: ["Material_T1_Stone", "Material_T3_Limestone"].map(itemOf), recipes: [{ name: "Recipe_Blueprint_Tool_Quickbuilder", displayName: "Block Blueprints" }] },
];
function itemOf(name: string) { const g = GAME.items.find((x) => x.name === name); return g?.displayName ? { name, displayName: g.displayName } : { name }; }

export const FIELDS: Record<string, CatalogFieldInfo[]> = {
  items: [
    { path: "maxStackSize", type: "uint16", writable: true, kind: "uint16" },
    { path: "rarity", type: "ItemRarity", writable: true, kind: "enum", enum: ["Common", "Uncommon", "Rare", "Epic", "Legendary"] },
    { path: "weight", type: "float32", writable: true, kind: "float32" },
    { path: "durability", type: "uint32", writable: true, kind: "uint32" },
    { path: "guid", type: "Guid", writable: false, kind: "guid" },
  ],
  recipes: [
    { path: "inputs", type: "BlobArray<RecipeItem>", writable: true, kind: "list" },
    { path: "outputs", type: "BlobArray<RecipeItem>", writable: true, kind: "list" },
    { path: "station", type: "CraftingStation", writable: true, kind: "ref" },
    { path: "duration", type: "Time", writable: true, kind: "time" },
  ],
  loot: [
    { path: "stackSizeMin", type: "uint16", writable: true, kind: "uint16" },
    { path: "stackSizeMax", type: "uint16", writable: true, kind: "uint16" },
  ],
  drops: [
    { path: "countMin", type: "uint32", writable: true, kind: "uint32" },
    { path: "countMax", type: "uint32", writable: true, kind: "uint32" },
    { path: "spawnProbability", type: "float32", writable: true, kind: "float32" },
    { path: "weight", type: "float32", writable: true, kind: "float32" },
  ],
};

/** What mods of the seeded library add (their "add" lists): by mod id. */
const ADDED: Record<string, { section: string; name: string; displayName?: string }[]> = {
  "new-items": [
    { section: "items", name: "Cinderbrand_Sword", displayName: "Cinderbrand Sword" },
    { section: "items", name: "Brass_Gear", displayName: "Brass Gear" },
    { section: "recipes", name: "Recipe_Cinderbrand_Sword", displayName: "Cinderbrand Sword" },
  ],
};

const SEED_LAYER = JSON.stringify({ format: 1, items: { "Material_T1_Wood_Logs": { maxStackSize: 500 } } }, null, 2) + "\n";
const MANIFEST_AUTHORS: Record<string, [string, string]> = {
  "new-items": ["frostdev", "https://github.com/frostdev-ops/new-items"],
  "night-sky": ["stargazer", "https://www.nexusmods.com/enshrouded/mods/313"],
  "weather-plus": ["cloudkeeper", "https://www.nexusmods.com/enshrouded/mods/316"],
  "more-hotbars": ["hotbarist", "https://github.com/hotbarist/more-hotbars"],
  "fps-unlock": ["framesmith", "https://github.com/framesmith/fps-unlock"],
};

export function studioMockHandlers(d: StudioMockDeps): Record<string, Handler> {
  const studios = new Map<string, MockStudio>();
  const requests: { cmd: string; args: Record<string, unknown> }[] = [];
  /** The game a test instance was activated in (pack id -> game id), as Rust's activation would be. */
  const testGame = new Map<string, string>();
  /** Removes the test instance like studio::drop_test: a game that has it loaded goes back to the pack's profile first;
   *  without that game, an active instance is refused. */
  const dropTest = (s: MockStudio, gameId: string | null) => {
    const old = s.source.testProfile;
    if (!old) return;
    const active = testGame.get(s.source.id);
    if (active && !gameId) err("refused", "the test instance is still the loaded profile of a game: load " + s.source.name + " again first");
    if (active && gameId) { d.activate(s.source.profile, gameId); testGame.delete(s.source.id); }
    d.profiles.delete(old);
  };
  let goneOnCrucible = FLAGS.has("gone");
  const signedIn = () => !!(d.handlers.community_status?.({}) as { account?: unknown } | undefined)?.account;

  const libName = (kind: ModKind, id: string) => d.library.find((e) => e.kind === kind && e.id === id)?.name ?? id;
  const libSide = (kind: ModKind, id: string, version: string) =>
    d.library.find((e) => e.kind === kind && e.id === id)?.versions.find((v) => v.version === version)?.side as StudioMod["side"] | undefined;

  const modsOf = (s: MockStudio): StudioMod[] => {
    const p = d.profiles.get(s.source.profile);
    if (!p) return [];
    return MOD_KINDS.flatMap((kind) => (p.mods[kind] ?? []).map((m): StudioMod => {
      const side = libSide(kind, m.id, m.version);
      const c = s.source.mods[m.id];
      const dep = c ? c.dep : m.enabled ? "required" : "optional";
      const def = dep === "optional" ? (c ? c.default ?? true : false) : undefined;
      const env: PackEnv = c?.env ?? (side === "client" ? { client: "required", server: "unsupported" } : side === "server" ? { client: "unsupported", server: "required" } : { client: "required", server: "required" });
      return { kind, id: m.id, name: libName(kind, m.id), version: m.version, hash: m.hash, enabled: m.enabled, side, dep, default: def, env, credit: s.credits[m.id] };
    }));
  };
  const view = (id: string): Studio => {
    const s = studios.get(id) ?? err("not_found", "No pack '" + id + "'.");
    const p = d.profiles.get(s.source.profile);
    // like Rust's revision: anything a build reads (the source, credits, layer files, the profile)
    const revision = String(hashOf(JSON.stringify([s.source.updated, s.credits, s.source.overrides, [...s.layer.entries()], p?.generation, p?.updated, p?.mods])));
    return clone({ dir: "/home/player/.local/share/kiln/studio/" + id, source: s.source, mods: modsOf(s), credits: s.credits, profileName: p?.name, layerFiles: s.layer.size, revision });
  };
  const make = (id: string, name: string, profile: string): MockStudio => ({
    source: { format: 1, kind: "pack-source", id, name, version: "1.0.0", profile, summary: null, description: null, authors: [], tags: [], changes: null, mods: {}, overrides: [],
      serverSettings: null, visibility: "link", gameBuilds: [], published: null, testProfile: null, testStarted: null, created: now(), updated: now() },
    credits: {}, layer: new Map(),
  });

  if (d.seeded && !FLAGS.has("empty") && d.profiles.has("ember-run")) {
    const s = make("ember-run", "Ember run", "ember-run");
    s.source.summary = "A cosy loot run with friends";
    s.source.overrides = [{ path: "big-stacks.json", side: "both", keys: ["multiplier"] }];
    s.layer.set("content/items.json", SEED_LAYER);
    for (const [id, [author, source]] of Object.entries(MANIFEST_AUTHORS)) if (id !== "night-sky") s.credits[id] = { author, source, match: id === "new-items" ? "kiln" : "manifest" };
    if (FLAGS.has("published")) {
      for (const m of modsOf(s)) s.credits[m.id] ??= { author: "someone", source: "https://www.nexusmods.com/enshrouded/mods/" + (300 + m.id.length), match: "nexus" };
      s.source.published = { pack: "pck_ember", slug: "ember-run", code: "H7QK-3MZP", url: "https://frostdev.io/kiln/crucible/ember-run", visibility: "public",
        versions: [{ version: "1.0.0", id: "pv_ember1", published: "2026-11-18T12:00:00Z", visibility: "public", file: "ember-run-1.0.0.kilnpack" }] };
      s.source.visibility = "public";
      s.source.version = "1.0.1";
    }
    studios.set(s.source.id, s);
  }

  const diag = (code: string, rule: string, severity: "error" | "warning" | "info", message: string, file: string): LintDiagnostic =>
    ({ file, code, severity, rule, message, span: { start: 0, end: 0 }, line: 1, col: 1, endLine: 1, endCol: 1, fixes: [], related: [] }) as unknown as LintDiagnostic;
  const lint = (s: MockStudio): LintReport => {
    const diagnostics: LintDiagnostic[] = [];
    const known = new Set([...Object.values(GAME).flat().flatMap((g) => [g.name, ...(g.id ? [g.id] : [])]), ...Object.values(ADDED).flat().map((a) => a.name)]);
    // the layer's own new items and recipes ("add") are known too
    for (const text of s.layer.values()) {
      try {
        const o = JSON.parse(text.replace(/\/\/[^\n]*/g, "")) as Record<string, { add?: { name?: string }[] }>;
        for (const body of Object.values(o ?? {})) for (const x of (Array.isArray(body?.add) ? body.add : [])) if (x?.name) known.add(x.name);
      } catch { /* reported below */ }
    }
    const refs = (list: unknown, where: string, path: string) => {
      for (const st of Array.isArray(list) ? list : []) {
        const item = typeof st === "string" ? st : (st as { item?: unknown })?.item;
        if (typeof item === "string" && item && !known.has(item)) diagnostics.push(diag("KL104", "unknown-ref", "warning", where + ": no item " + item + " in the game or the pack's mods", "layer/" + path));
        // a category input: one of the catalog's itemCategories (by id, as a number: 0xEE.. = 0xee..)
        const category = typeof st === "object" && st ? (st as { category?: unknown }).category : undefined;
        const num = (v: unknown) => (typeof v === "number" ? v : typeof v === "string" && /^(0[xX])?[0-9a-fA-F]+$/.test(v.trim()) ? parseInt(v.trim().replace(/^0[xX]/, ""), 16) : NaN);
        if (category !== undefined && catalogOk && !CATEGORIES.some((c) => num(c.id) === num(category))) diagnostics.push(diag("KL104", "unknown-ref", "warning", where + ": no item category " + String(category) + " in the game", "layer/" + path));
      }
    };
    for (const [path, text] of s.layer) {
      let v: unknown;
      try { v = JSON.parse(text.replace(/\/\/[^\n]*/g, "")); } catch (e) {
        diagnostics.push(diag("KL001", "json-syntax", "error", "Not valid JSON: " + (e as Error).message, "layer/" + path));
        continue;
      }
      const o = (v ?? {}) as Record<string, Record<string, unknown>>;
      for (const section of ["items", "recipes", "loot", "drops"]) {
        for (const sel of Object.keys(o[section] ?? {})) {
          if (sel === "add" || sel.includes("*") || sel.startsWith("@")) continue;
          if (!known.has(sel)) diagnostics.push(diag("KL103", "unknown-name", "warning", section + ": no entry " + sel + " in the game or the pack's mods", "layer/" + path));
          else if (hiddenEntry(sel)) diagnostics.push(diag("KL407", "hidden-entry", "info", "\"" + sel + "\" is one of the game's old, test or unused entries (its name says so): it may do nothing in the game", "layer/" + path));
        }
      }
      for (const [sel, body] of Object.entries(o.recipes ?? {})) {
        const list = sel === "add" ? (Array.isArray(body) ? body : []) : [body];
        for (const r of list as { inputs?: unknown; outputs?: unknown; name?: string }[]) { refs(r?.inputs, "recipes " + (r?.name ?? sel) + " inputs", path); refs(r?.outputs, "recipes " + (r?.name ?? sel) + " outputs", path); }
      }
      for (const [sel, body] of Object.entries(o.drops ?? {})) {
        refs(Object.keys(((body ?? {}) as { items?: Record<string, unknown> }).items ?? {}).filter((k) => !k.includes("*")), "drops " + sel + " items", path);
      }
    }
    const errors = diagnostics.filter((x) => x.severity === "error").length, infos = diagnostics.filter((x) => x.severity === "info").length;
    return { format: 1, tool: "kiln-lint", version: "mock", ok: errors === 0, checked: [...s.layer.keys()].map((p) => "layer/" + p),
      summary: { errors, warnings: diagnostics.length - errors - infos, infos, hints: 0 }, catalog: !catalogOk ? null : { path: "catalog/client.json", side: "client", build: "26430253" }, diagnostics };
  };
  const layerPathOk = (p: string) => /^content\/[A-Za-z0-9._-]+\.jsonc?$/.test(p) && !p.slice(8).startsWith(".");
  const of = (a: Record<string, unknown>) => studios.get(S(a.packId)) ?? err("not_found", "No pack '" + S(a.packId) + "'.");

  const CAPTURE: StudioCaptureFile[] = [
    { path: "big-stacks.json", mod: "big-stacks", mode: "merge-patch", defaults: true, whole: false, side: "both", size: 120, keys: [
      { key: "multiplier", value: 4, default: 2, chosen: false, personal: false },
      { key: "materialsOnly", value: false, default: true, chosen: false, personal: false },
      { key: "hotkey", value: "F9", default: "F1", chosen: false, personal: true },
    ] },
    { path: "night-sky.json", mod: "night-sky", mode: "merge-patch", defaults: true, whole: false, side: "both", size: 64, keys: [
      { key: "stars.brightness", value: 1.4, default: 1, chosen: false, personal: false },
    ] },
    { path: "emf/beastmaster.ini", mod: "beastmaster", mode: "ini", defaults: false, whole: false, side: "both", size: 80, keys: [
      { key: "Pets.maxPets", value: "3", chosen: false, personal: false },
      { key: "Pets.followDistance", value: "6", chosen: false, personal: false },
    ] },
    { path: "discord-bridge.json", mod: "discord-bridge", mode: "merge-patch", defaults: false, whole: false, side: "both", size: 90, keys: [
      { key: "channel", value: "general", chosen: false, personal: false },
      { key: "token", value: "MTA4…", chosen: false, personal: false, secret: "discord-bridge.json: token looks like a secret (token)" },
    ] },
    { path: "notes.txt", mode: "replace", defaults: false, keys: [], whole: false, side: "both", size: 40 },
  ];
  const captureOf = (s: MockStudio): StudioCaptureFile[] => CAPTURE.map((f) => {
    const spec = s.source.overrides.find((o) => o.path === f.path);
    return { ...clone(f), side: spec?.side ?? "both", whole: !!spec && spec.keys.length === 0,
      keys: f.keys.map((k) => ({ ...k, chosen: !k.secret && !!spec && (spec.keys.length === 0 || spec.keys.includes(k.key)) })) };
  });

  const needsOf = (s: MockStudio, r: StudioPublishRequest): string[] => {
    const out: string[] = [];
    if (!/^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/.test(r.version.trim())) out.push(r.version + " is not a version like 1.2.0");
    if (s.source.published?.versions.some((v) => v.version === r.version.trim())) out.push("version " + r.version.trim() + " is published already: choose a newer one");
    const mods = modsOf(s);
    if (!mods.length) out.push("the pack has no mods");
    if (r.visibility === "public") {
      const missing = mods.filter((m) => !(m.credit?.author?.trim() && /^https:\/\/./.test(m.credit?.source ?? ""))).map((m) => m.name);
      if (missing.length) out.push("credits for " + missing.join(", "));
      if ((s.source.summary ?? "").trim().length < 10) out.push("a summary of at least 10 characters");
      if (!r.attest) out.push("confirm that you credited every mod and may share these files");
    }
    return out;
  };

  const h: Record<string, Handler> = {
    studio_list: (): StudioSummary[] => [...studios.values()].map((s) => ({ id: s.source.id, name: s.source.name, version: s.source.version, profile: s.source.profile,
      mods: modsOf(s).length, updated: s.source.updated, published: s.source.published ?? undefined })).sort((a, b) => b.updated.localeCompare(a.updated)),
    studio_create: (a) => {
      const r = a.request as StudioNewPack;
      const name = S(r.name).trim();
      if (!name) err("invalid_argument", "the pack needs a name");
      const base = r.id ?? (name.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 59) || "pack");
      let id = base, n = 2;
      while (studios.has(id)) id = base + "-" + n++;
      let pid = r.from ?? null;
      if (pid && !d.profiles.has(pid)) err("not_found", "No profile '" + pid + "'.");
      if (!pid) {
        pid = d.slug(name);
        let m = 2;
        while (d.profiles.has(pid)) pid = d.slug(name) + "-" + m++;
        d.profiles.set(pid, { format: 1, id: pid, name, created: now(), updated: now(), generation: 1, notes: "", mods: emptyMods(), dropins: [], dataDir: "shared", server: null });
      }
      studios.set(id, make(id, name, pid));
      return view(id);
    },
    studio_get: (a) => view(of(a).source.id),
    studio_save: (a) => {
      const s = of(a);
      const next = clone(a.source as PackSourceDoc);
      if (!next.name?.trim()) err("invalid_argument", "the pack needs a name (at most 128 characters)");
      if (!/^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/.test(next.version?.trim() ?? "")) err("invalid_argument", next.version + " is not a version like 1.2.0");
      const gs = next.serverSettings as Record<string, unknown> | null | undefined;
      if (gs && Object.keys(gs).some((k) => k !== "gameSettings")) err("invalid_argument", "server settings hold gameSettings only ({\"gameSettings\": {...}})");
      s.source = { ...next, id: s.source.id, profile: s.source.profile, created: s.source.created, published: s.source.published, testProfile: s.source.testProfile,
        testStarted: s.source.testStarted, name: next.name.trim(), version: next.version.trim(), updated: now() };
      return view(s.source.id);
    },
    studio_delete: (a) => { const s = of(a); dropTest(s, a.gameId ? S(a.gameId) : null); studios.delete(s.source.id); return null; },
    studio_credits_set: (a) => {
      const s = of(a);
      const c = (a.credits ?? {}) as Record<string, PackCredit>;
      const clean: Record<string, PackCredit> = {};
      for (const [id, x] of Object.entries(c)) {
        const author = S(x.author).trim(), source = S(x.source).trim();
        if (!author && !source) continue;
        if (source && !source.startsWith("https://")) err("invalid_argument", id + ": the source must be an https:// link");
        clean[id] = { author, source, ...(x.match ? { match: x.match } : {}) };
      }
      s.credits = clean;
      s.source.updated = now();
      return view(s.source.id);
    },
    studio_capture: (a) => captureOf(of(a)),
    studio_capture_set: (a) => {
      const s = of(a);
      const offered = captureOf(s);
      const out = [];
      for (const c of (a.choices ?? []) as StudioCaptureChoice[]) {
        const f = offered.find((x) => x.path === c.path) ?? err("not_found", c.path + ": not a settings file of this pack's profile");
        if (c.whole || f.mode === "replace") {
          const sec = f.keys.find((k) => k.secret);
          if (sec) err("refused", c.path + ": " + sec.key + " may hold private data; choose keys instead of the whole file");
          out.push({ path: c.path, side: c.side ?? "both", keys: [] });
          continue;
        }
        if (!c.keys.length) continue;
        for (const k of c.keys) {
          const ck = f.keys.find((x) => x.key === k) ?? err("not_found", c.path + ": no key " + k + " that differs from the default");
          if (ck.secret) err("refused", c.path + ": " + k + " may hold private data (" + ck.secret + ")");
        }
        out.push({ path: c.path, side: c.side ?? "both", keys: [...c.keys] });
      }
      s.source.overrides = out;
      s.source.updated = now();
      return view(s.source.id);
    },
    studio_order: (a): StudioOrder => {
      const s = of(a);
      const p = d.profiles.get(s.source.profile) ?? err("not_found", "No profile.");
      const on = p.mods.kiln.filter((m) => m.enabled), off = p.mods.kiln.filter((m) => !m.enabled);
      return { kiln: [...on.map((m) => ({ id: m.id, name: libName("kiln", m.id), loads: true, layer: false })),
        { id: "pack." + s.source.id, name: s.source.name + " (pack layer)", loads: true, layer: true },
        ...off.map((m) => ({ id: m.id, name: libName("kiln", m.id), loads: false, reason: "Switched off in the profile.", layer: false }))],
        late: [...(p.late ?? [])], warnings: [] };
    },
    studio_layer_files: (a): StudioLayerFile[] => [...of(a).layer.entries()].sort(([x], [y]) => x.localeCompare(y)).map(([path, text]) => ({ path, size: new Blob([text]).size })),
    studio_layer_read: (a) => of(a).layer.get(S(a.path)) ?? err("not_found", S(a.path) + ": no such layer file"),
    studio_layer_write: (a) => {
      const s = of(a), path = S(a.path), text = S(a.text);
      if (!layerPathOk(path)) err("invalid_argument", path + ": layer files are content/<name>.json");
      if (/"(token|password|secret)"\s*:\s*"[^"]+"/i.test(text)) err("refused", path + " may hold private data");
      s.layer.set(path, text);
      s.source.updated = now();
      return lint(s);
    },
    studio_layer_delete: (a) => { const s = of(a); s.layer.delete(S(a.path)); return lint(s); },
    studio_layer_set: (a) => {
      const s = of(a);
      const path = "content/pack.json";
      const o = JSON.parse((s.layer.get(path) ?? '{ "format": 1 }').replace(/\/\/[^\n]*/g, "")) as Record<string, Record<string, Record<string, Json>>>;
      const sec = (o[S(a.section)] ??= {});
      const sel = (sec[S(a.selector)] ??= {});
      if (a.value === null) delete sel[S(a.field)]; else sel[S(a.field)] = a.value as Json;
      s.layer.set(path, "{\n  // Set by Pack Studio (Load order: conflicts)\n" + JSON.stringify(o, null, 2).slice(2));
      return lint(s);
    },
    studio_layer_lint: (a) => lint(of(a)),
    studio_pick: (a): StudioPickHit[] => {
      const s = of(a);
      const section = S(a.section), query = S(a.query).trim().toLowerCase(), limit = Number(a.limit ?? 50) || 50;
      const p = d.profiles.get(s.source.profile);
      const added = Object.entries(ADDED).filter(([mod]) => p?.mods.kiln.some((m) => m.id === mod)).flatMap(([mod, list]) => list.map((x) => ({ ...x, from: mod })));
      for (const text of s.layer.values()) {
        try {
          const o = JSON.parse(text.replace(/\/\/[^\n]*/g, "")) as Record<string, { add?: { name?: string; displayName?: string }[] }>;
          for (const [sec, body] of Object.entries(o)) for (const x of body?.add ?? []) if (x.name) added.push({ section: sec, name: x.name, displayName: x.displayName, from: "pack." + s.source.id });
        } catch { /* lint says */ }
      }
      const match = (x: { name: string; displayName?: string; id?: string }) => !query || x.name.toLowerCase().includes(query) || !!x.displayName?.toLowerCase().includes(query) || x.id?.toLowerCase() === query;
      const own = added.filter((x) => x.section === section && match(x)).map((x) => ({ section, name: x.name, displayName: x.displayName, from: x.from }));
      // hidden game entries only when asked, or by their exact name (typed or pasted)
      const shown = (x: { name: string }) => !hiddenEntry(x.name) || a.hidden === true || x.name.toLowerCase() === query;
      const game = !catalogOk ? [] : (GAME[section] ?? []).filter((x) => match(x) && shown(x))
        .map((x) => ({ section, name: x.name, id: x.id, displayName: x.displayName, ...(hiddenEntry(x.name) ? { hidden: true } : {}) }));
      return [...own, ...game.filter((g) => !own.some((o) => o.name === g.name))].slice(0, limit);
    },
    studio_categories: (): StudioItemCategory[] => (!catalogOk ? [] : clone(CATEGORIES)),
    studio_fields: (a) => (!catalogOk ? [] : FIELDS[S(a.section)] ?? []),
    studio_catalog_status: (): StudioCatalogStatus => clone(catalog),
    // like catalog::auto: answers "building" at once; the first try without a catalog is refused (Steam updating), the
    // next one (the button) makes it
    studio_catalog_ensure: (): StudioCatalogStatus => {
      if (catalog.state === "building") return clone(catalog);
      const attempt = ++catalogAttempts;
      catalog = { state: "building", exists: catalogOk };
      setTimeout(() => {
        if (catalogOk) catalog = { state: "ready", exists: true, path: CATALOG_PATH, built: false };
        else if (attempt === 1) catalog = { state: "failed", exists: false, path: CATALOG_PATH, error: { code: "steam_updating", message: "Steam is updating the game. Try again when it is done." } };
        else { catalogOk = true; catalog = { state: "ready", exists: true, path: CATALOG_PATH, built: true, seconds: 0.9 }; }
      }, catalogOk ? 40 : 900);
      return clone(catalog);
    },
    studio_analyze: (a) => {
      const s = of(a);
      return d.startTask("studio_analyze", "Checking the pack", ["building", "checking the pack"], (): PackCheckReport => {
        const mods = modsOf(s);
        const credits = mods.map((m) => {
          const known = MANIFEST_AUTHORS[m.id];
          return { kind: m.kind, id: m.id, version: m.version, kdh1: m.hash, author: known?.[0] ?? null, source: known?.[1] ?? null,
            match: m.id === "new-items" ? "kiln" : known ? "manifest" : "manual", confidence: m.id === "new-items" ? "certain" : known ? "unverified" : "none" };
        });
        const layerSets = [...s.layer.values()].some((t) => t.includes("Material_T1_Wood_Logs"));
        const twigs = [...s.layer.values()].some((t) => t.includes("Material_T1_Twigs"));
        const conflicts = [
          ...(layerSets ? [{ section: "items", field: "maxStackSize", conditional: false,
            earlier: { mod: "big-stacks", file: "content/stacks.json", selector: "Material_T1_*", value: 1000 },
            later: { mod: "pack." + s.source.id, file: "content/items.json", selector: "Material_T1_Wood_Logs", value: 500 } }] : []),
          ...(twigs ? [] : [{ section: "items", field: "weight", conditional: false,
            earlier: { mod: "new-items", file: "content/items.json", selector: "Material_T1_Twigs", value: 0.2 },
            later: { mod: "weather-plus", file: "content/tweaks.json", selector: "Material_T1_Twigs", value: 0.5 } }]),
        ];
        const code = mods.filter((m) => m.kind === "native" || m.kind === "plugin");
        return { format: 1, kind: "kiln.pack-check", tool: "kiln", version: "mock", ok: true, complete: true,
          pack: { id: s.source.id, name: s.source.name, version: s.source.version },
          summary: { errors: 0, warnings: conflicts.length, notes: code.length }, catalog: !!catalogOk,
          findings: [...conflicts.map(() => ({ level: "warning" as const, code: "KL701", message: "items Material_T1_Wood_Logs.maxStackSize: big-stacks sets 1000, the pack layer sets 500 (the later mod wins: the pack layer)" })),
            ...code.map((m) => ({ level: "note" as const, code: "code-native", message: m.name + " carries native code", mod: m.id }))],
          mods: mods.map((m) => ({ kind: m.kind, id: m.id, version: m.version, hash: m.hash, name: m.name, code: { managed: [], native: m.kind === "native" ? [m.id + ".dll"] : [], scripts: [], touches: [] }, blocked: false })),
          conflicts, credits };
      }, 300);
    },
    studio_test: (a) => {
      const s = of(a);
      const gameId = a.gameId ? S(a.gameId) : null;
      return d.startTask("studio_test", "Making the test instance", ["building the pack", "installing the test instance", "activating"], () => {
        dropTest(s, gameId);
        const src = d.profiles.get(s.source.profile)!;
        const pid = d.slug(s.source.name + " (test)");
        const prof: Profile = { ...clone(src), id: pid, name: s.source.name + " (test)", created: now(), updated: now(), generation: 1, dataDir: "profile" };
        prof.mods.kiln.push({ id: "pack." + s.source.id, version: s.source.version, hash: "kdh1:" + "0".repeat(64), enabled: true });
        d.profiles.set(pid, prof);
        s.source.testProfile = pid;
        s.source.testStarted = now();
        const activation = gameId ? d.activate(pid, gameId) : null;
        if (gameId) testGame.set(s.source.id, gameId);
        return { profile: prof, instance: { pack: s.source.id, name: s.source.name, version: s.source.version, layer: "pack." + s.source.id, optional: {}, installed: now() }, imported: [], overrides: s.source.overrides.map((o) => o.path), backup: null, activation };
      }, 300);
    },
    studio_test_report: (a): StudioTestReport => {
      const s = of(a);
      return s.source.testProfile ? { profile: s.source.testProfile, ran: true,
        conflicts: ["[W 21:04:11.210] [kiln] conflict: items Material_T1_Wood_Logs.maxStackSize: big-stacks sets 1000, pack." + s.source.id + " sets 500 (the later one wins: pack." + s.source.id + ")"],
        problems: ["[E 21:04:12.002] [mod:weather-plus] weather table 'Storm_Ash' not found"] } : { ran: false, conflicts: [], problems: [] };
    },
    studio_test_end: (a) => { const s = of(a); dropTest(s, a.gameId ? S(a.gameId) : null); s.source.testProfile = null; s.source.testStarted = null; s.source.updated = now(); return null; },
    studio_build: (a) => {
      const s = of(a);
      return d.startTask("studio_build", "Saving the pack as a file", ["hashing", "packing"], (): BuiltPack => ({
        file: S(a.outDir) + "/" + s.source.id + "-" + s.source.version + ".kilnpack", sha256: "ab".repeat(32), size: 18_422_113,
        index: { format: 2, kind: "pack", id: s.source.id, name: s.source.name, version: s.source.version, mods: [], order: { kiln: [], late: [] } } as never, notes: [] }), 300);
    },
    studio_publish_check: (a) => needsOf(of(a), a.request as StudioPublishRequest),
    studio_publish: (a) => {
      const s = of(a);
      const r = a.request as StudioPublishRequest;
      if (!signedIn()) err("auth_failed", "Sign in with Discord to publish.");
      const needs = needsOf(s, r);
      if (needs.length) err("refused", "Before publishing: " + needs.join("; ") + ".", needs);
      if (goneOnCrucible && s.source.published && !r.fresh) err("refused", "This pack is no longer on Crucible. (Publish as a new pack to put it on Crucible again.)", { gone: true });
      return d.startTask("studio_publish", "Publishing to Crucible", ["building the pack", "asking Crucible for upload slots", "uploading 4.1 MB of 18.4 MB", "confirming the credits"], (): StudioPublishResult => {
        const prev = r.fresh ? null : s.source.published;
        if (r.fresh) goneOnCrucible = false;
        const packId = prev?.pack ?? "pck_" + s.source.id.replace(/[^a-z0-9]/g, "") + (r.fresh ? "2" : "");
        // ?studio=notready: Crucible's checks still run, so the pack stays link-only and says why
        const notReady = FLAGS.has("notready") && r.visibility === "public" && prev?.visibility !== "public";
        const vis = notReady ? "link" : r.visibility;
        const vId = "pv_" + s.source.id.replace(/[^a-z0-9]/g, "") + (prev?.versions.length ?? 0 + 1) + Date.now() % 1000;
        const pack: CruciblePack = { id: packId, slug: s.source.id, code: prev?.code ?? "H7QK-3MZP", url: vis === "public" ? "https://frostdev.io/kiln/crucible/" + s.source.id : "https://frostdev.io/kiln/crucible/p/H7QK-3MZP",
          title: s.source.name, summary: s.source.summary ?? "", icon: null, owner: { id: "731204485922648124", name: "emberfox" }, visibility: vis, upvotes: 0, comments: 0, installs: 0, mods: modsOf(s).length, builds: r.gameBuilds ?? [] };
        const version: CrucibleVersion = { id: vId, number: (prev?.versions.length ?? 0) + 1, version: r.version.trim(), changes: r.changes ?? null, size: 18_422_113, published: now(), status: "ready" } as CrucibleVersion;
        const file = s.source.id + "-" + r.version.trim() + ".kilnpack";
        s.source.published = { pack: packId, slug: s.source.id, code: pack.code, url: pack.url, visibility: vis,
          versions: [...(s.source.published?.versions ?? []), { version: r.version.trim(), id: vId, published: now(), visibility: vis, file }] };
        s.source.visibility = r.visibility;
        s.source.updated = now();
        const [ma, mi, pa] = r.version.trim().split(/[.-]/).map(Number);
        s.source.version = ma + "." + mi + "." + (pa + 1);
        s.source.changes = null;
        return { pack, version, files: modsOf(s).length + 2, uploaded: 4_300_000, skipped: 14_122_113, public: vis === "public", needs: notReady ? ["the checks are still running: try again in a minute"] : [], file };
      }, 350);
    },
    studio_set_visibility: (a) => {
      const s = of(a);
      if (!signedIn()) err("auth_failed", "Sign in with Discord to publish.");
      const p = s.source.published ?? err("not_found", "publish the pack first");
      const to = S(a.visibility) as "link" | "public";
      if (to === "public") {
        const why = needsOf(s, { version: "0.0.0-x", visibility: "public", attest: !!a.attest }).filter((n) => !n.includes("version"));
        if (why.length) err("refused", "Before going public: " + why.join("; ") + ".", why);
      }
      p.visibility = to;
      s.source.visibility = to;
      s.source.updated = now();
      const pack: CruciblePack = { id: p.pack, slug: s.source.id, code: p.code ?? "H7QK-3MZP", url: to === "public" ? "https://frostdev.io/kiln/crucible/" + s.source.id : "https://frostdev.io/kiln/crucible/p/" + (p.code ?? "H7QK-3MZP"),
        title: s.source.name, summary: s.source.summary ?? "", icon: null, owner: { id: "731204485922648124", name: "emberfox" }, visibility: to, upvotes: 0, comments: 0, installs: 0, mods: modsOf(s).length, builds: [] };
      return { pack, public: to === "public", needs: [] };
    },
    studio_server_pack: (a) => {
      const s = of(a);
      const v = S(a.version);
      if (!s.source.published?.versions.some((x) => x.version === v)) err("not_found", "version " + v + " of this pack is not kept on this PC");
      return d.startTask("studio_server_pack", "Building the server pack", ["placing the mods on the server", "building the player modpack"], () => ({
        mods: [], imported: [], overrides: s.source.overrides.map((o) => o.path), settings: !!s.source.serverSettings, leftOut: [],
        modpack: { file: "/srv/ember-isles/modpacks/" + s.source.id + "-" + v + ".zip", sha256: "cd".repeat(32), size: 12_000_000, manifest: { mods: [] } } }), 300);
    },
  };
  // every call is recorded for the tests
  const wrapped: Record<string, Handler> = {};
  for (const [k, fn] of Object.entries(h)) wrapped[k] = (args) => { requests.push({ cmd: k, args: clone(args ?? {}) }); return fn(args ?? {}); };
  if (typeof window !== "undefined") (window as unknown as { __kilnStudioMock: unknown }).__kilnStudioMock = { requests: () => clone(requests), state: () => clone([...studios.values()].map((s) => ({ ...s, layer: Object.fromEntries(s.layer) }))) };
  return wrapped;
}
