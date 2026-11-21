// The data editors' model (DESIGN 3.3): reading a layer file (JSONC), the rules of a section as editable rows and back,
// and the values Kiln's content packs take (docs/kiln/services/content.md "Data packs"; schema/content-pack.schema.json).
// Each editor owns one part of one file and keeps everything else of the file as it was ("format": 1 first).
import { parse, type ParseError } from "jsonc-parser";
import type { CatalogFieldInfo, Json } from "../../api/commands";

export type Obj = Record<string, Json>;
export const isObj = (v: unknown): v is Obj => !!v && typeof v === "object" && !Array.isArray(v);

/** The file each editor writes (one per editor; Items and New items share items.json: rules and "add"). */
export const FILES = {
  items: "content/items.json",
  recipes: "content/recipes.json",
  loot: "content/loot.json",
  drops: "content/drops.json",
  added: "content/items.json",
} as const;

export interface Doc { ok: boolean; value: Obj; error?: string }
/** A layer file as an object (comments and trailing commas allowed, as kiln.content reads it). Missing: an empty doc. */
export function readDoc(text: string | undefined): Doc {
  if (text === undefined || text.trim() === "") return { ok: true, value: { format: 1 } };
  const errors: ParseError[] = [];
  const v = parse(text, errors, { allowTrailingComma: true, disallowComments: false });
  if (errors.length || !isObj(v)) return { ok: false, value: {}, error: errors.length ? "offset " + errors[0].offset : "not an object" };
  return { ok: true, value: v as Obj };
}
/** The doc as written back: "format" first, two-space JSON. */
export function writeDoc(doc: Obj): string {
  const { format, ...rest } = doc;
  return JSON.stringify({ format: format ?? 1, ...rest }, null, 2) + "\n";
}
/** Puts a section back (removed when empty). */
export function withSection(doc: Obj, section: string, body: Obj): Obj {
  const out: Obj = { ...doc };
  if (Object.keys(body).length) out[section] = body; else delete out[section];
  return out;
}

// ---------------------------------------------------------------- fields and values

export type ValueKind = "number" | "integer" | "enum" | "bool" | "other";
export function kindOf(f: CatalogFieldInfo | undefined): ValueKind {
  if (!f) return "other";
  if (f.enum?.length) return "enum";
  const k = (f.kind ?? f.type ?? "").toLowerCase();
  if (/^bool/.test(k)) return "bool";
  if (/^u?int\d*$/.test(k) || k === "integer") return "integer";
  if (/^(float\d*|double|number|time)$/.test(k)) return "number";
  return "other";
}

/** Fields the editors offer even where the catalog lists none (content.md: loot and drop stacks). */
export const FALLBACK_FIELDS: Record<string, CatalogFieldInfo[]> = {
  loot: [
    { path: "stackSizeMin", type: "uint16", writable: true, kind: "uint16" },
    { path: "stackSizeMax", type: "uint16", writable: true, kind: "uint16" },
    { path: "stackSizeMaxScaled", type: "uint16", writable: true, kind: "uint16" },
  ],
  dropItems: [
    { path: "countMin", type: "uint32", writable: true, kind: "uint32" },
    { path: "countMax", type: "uint32", writable: true, kind: "uint32" },
    { path: "spawnProbability", type: "float32", writable: true, kind: "float32" },
    { path: "weight", type: "float32", writable: true, kind: "float32" },
  ],
};
export function mergeFields(a: CatalogFieldInfo[], b: CatalogFieldInfo[] = []): CatalogFieldInfo[] {
  const out = [...a];
  for (const f of b) if (!out.some((x) => x.path === f.path)) out.push(f);
  return out;
}

/** A number change relative to the game's value: {"mul": 4, "max": 9999, "raiseOnly": true}. */
export interface NumOp { mode: "set" | "mul" | "add"; value: number; min?: number; max?: number; raiseOnly?: boolean; lowerOnly?: boolean }
const OP_KEYS = new Set(["mul", "add", "min", "max", "raiseOnly", "lowerOnly", "set"]);
/** A value as a number op, or null when it is something else (a list, two of mul/add/set...). */
export function numOp(v: Json | undefined): NumOp | null {
  if (typeof v === "number") return { mode: "set", value: v };
  if (!isObj(v) || !Object.keys(v).every((k) => OP_KEYS.has(k))) return null;
  const modes = (["set", "mul", "add"] as const).filter((m) => m in v);
  if (modes.length !== 1 || typeof v[modes[0]] !== "number") return null;
  const n = (k: string) => (typeof v[k] === "number" ? (v[k] as number) : undefined);
  return { mode: modes[0], value: v[modes[0]] as number, min: n("min"), max: n("max"), raiseOnly: v.raiseOnly === true || undefined, lowerOnly: v.lowerOnly === true || undefined };
}
export function opValue(o: NumOp): Json {
  const extra: Obj = {};
  if (o.min !== undefined) extra.min = o.min;
  if (o.max !== undefined) extra.max = o.max;
  if (o.raiseOnly) extra.raiseOnly = true;
  if (o.lowerOnly) extra.lowerOnly = true;
  if (o.mode === "set") return Object.keys(extra).length ? { set: o.value, ...extra } : o.value;
  return { [o.mode]: o.value, ...extra };
}

/** A typed word as a value: true / false, a number, JSON ([..], {..}, "..."), else the text. */
export function parseLoose(text: string): Json {
  const s = text.trim();
  if (s === "true") return true;
  if (s === "false") return false;
  if (/^-?\d+(\.\d+)?$/.test(s)) return Number(s);
  if (/^[[{"]/.test(s)) { try { return JSON.parse(s) as Json; } catch { /* the text */ } }
  return text;
}
export const showLoose = (v: Json | undefined): string => (v === undefined ? "" : typeof v === "string" ? v : JSON.stringify(v));

// ---------------------------------------------------------------- rules (items, loot)

/** "<selector>": {"@where": {...}, field: value, ...}. extra: other "@" keys (@refs), kept. */
export interface Rule { sel: string; where: [string, Json][]; fields: [string, Json][]; extra: Obj }
export function rulesOf(section: Json | undefined): Rule[] {
  if (!isObj(section)) return [];
  return Object.entries(section).filter(([k]) => k !== "add").map(([sel, body]) => {
    const b = isObj(body) ? body : {};
    const extra: Obj = {};
    const fields: [string, Json][] = [];
    for (const [k, v] of Object.entries(b)) {
      if (k === "@where") continue;
      if (k.startsWith("@")) extra[k] = v; else fields.push([k, v]);
    }
    return { sel, where: isObj(b["@where"]) ? Object.entries(b["@where"] as Obj) : [], fields, extra };
  });
}
export function ruleBody(r: Rule): Obj {
  const out: Obj = {};
  const where = r.where.filter(([k]) => k.trim());
  if (where.length) out["@where"] = Object.fromEntries(where);
  Object.assign(out, r.extra);
  for (const [k, v] of r.fields) if (k.trim()) out[k] = v;
  return out;
}
/** Selectors used twice (a JSON object holds one rule per key). */
export function duplicates(sels: string[]): Set<string> {
  const seen = new Set<string>(), dup = new Set<string>();
  for (const s of sels.map((x) => x.trim())) { if (seen.has(s)) dup.add(s); seen.add(s); }
  return dup;
}

// ---------------------------------------------------------------- stacks (recipes, drops)

/** An input or output: an item, or (inputs) any item of a category of the game ({"category": "0x..", count}; rule stacks
 *  may write the id as a number, kept as written). category "": a category stack whose category is not chosen yet. */
export interface Stack { item: string; count: number; category?: string | number }
/** A stack's category, when it is one (a chosen or still empty category, no item). */
export const isCategoryStack = (s: Stack): boolean => s.category !== undefined && !s.item.trim();
const hasCategory = (s: Stack): boolean => s.category !== undefined && s.category !== "";
export function stacksOf(v: Json | undefined): Stack[] | null {
  if (!Array.isArray(v)) return null;
  return v.map((x) => typeof x === "string" ? { item: x, count: 1 }
    : isObj(x) ? { item: typeof x.item === "string" ? x.item : "", count: typeof x.count === "number" ? x.count : 1,
      ...(typeof x.category === "string" || typeof x.category === "number" ? { category: x.category } : x.category !== undefined && x.category !== null ? { category: String(x.category) } : {}) }
      : { item: "", count: 1 });
}
/** The stacks as written: {item, count}, {category, count}; a stack holding both (a rule may) keeps both; empty ones
 *  (no item, no category chosen) are left out. */
export const stacksValue = (s: Stack[]): Json => s.filter((x) => x.item.trim() || hasCategory(x)).map((x): Obj => (
  !x.item.trim() ? { category: x.category as string | number, count: x.count }
    : hasCategory(x) ? { item: x.item.trim(), category: x.category as string | number, count: x.count } : { item: x.item.trim(), count: x.count }));

/** A recipe rule: "<recipe>": {inputs, outputs, craftingDuration, ...the rest kept}. */
export interface RecipeRule { sel: string; inputs: Stack[] | null; outputs: Stack[] | null; duration: number | null; rest: Obj }
export function recipeRulesOf(section: Json | undefined): RecipeRule[] {
  if (!isObj(section)) return [];
  return Object.entries(section).filter(([k]) => k !== "add").map(([sel, body]) => {
    const b = isObj(body) ? { ...body } : {};
    const inputs = stacksOf(b.inputs), outputs = stacksOf(b.outputs);
    if (inputs) delete b.inputs;
    if (outputs) delete b.outputs;
    const duration = typeof b.craftingDuration === "number" ? b.craftingDuration : null;
    if (duration !== null) delete b.craftingDuration;
    return { sel, inputs, outputs, duration, rest: b };
  });
}
export function recipeRuleBody(r: RecipeRule): Obj {
  const out: Obj = { ...r.rest };
  if (r.inputs) out.inputs = stacksValue(r.inputs);
  if (r.outputs) out.outputs = stacksValue(r.outputs);
  if (r.duration !== null) out.craftingDuration = r.duration;
  return out;
}

/** A new recipe ("add"): guid, name, station, inputs, outputs, duration; other keys kept. */
export interface RecipeDef { guid: string; name: string; station: string; inputs: Stack[]; outputs: Stack[]; duration: number | null; rest: Obj }
export function recipeDefsOf(section: Json | undefined): RecipeDef[] {
  const add = isObj(section) && Array.isArray(section.add) ? section.add : [];
  return add.filter(isObj).map((d) => {
    const { guid, name, station, inputs, outputs, duration, ...rest } = d;
    return { guid: String(guid ?? ""), name: String(name ?? ""), station: typeof station === "string" ? station : "", inputs: stacksOf(inputs) ?? [], outputs: stacksOf(outputs) ?? [],
      duration: typeof duration === "number" ? duration : null, rest };
  });
}
export function recipeDefValue(d: RecipeDef): Obj {
  const out: Obj = { guid: d.guid, name: d.name.trim() };
  if (d.station) out.station = d.station;
  Object.assign(out, d.rest);
  out.inputs = stacksValue(d.inputs);
  out.outputs = stacksValue(d.outputs);
  if (d.duration !== null) out.duration = d.duration;
  return out;
}

// ---------------------------------------------------------------- drops

export interface DropRule { sel: string; count: Json | undefined; chance: Json | undefined; items: [string, [string, Json][]][]; rest: Obj }
export function dropRulesOf(section: Json | undefined): DropRule[] {
  if (!isObj(section)) return [];
  return Object.entries(section).map(([sel, body]) => {
    const { count, chance, items, ...rest } = isObj(body) ? body : ({} as Obj);
    const list: [string, [string, Json][]][] = isObj(items) ? Object.entries(items).map(([k, v]) => [k, isObj(v) ? Object.entries(v) : []]) : [];
    return { sel, count, chance, items: list, rest: isObj(items) || items === undefined ? rest : { ...rest, items } };
  });
}
export function dropRuleBody(r: DropRule): Obj {
  const out: Obj = { ...r.rest };
  if (r.count !== undefined) out.count = r.count;
  if (r.chance !== undefined) out.chance = r.chance;
  const items = r.items.filter(([k]) => k.trim());
  if (items.length) out.items = Object.fromEntries(items.map(([k, f]) => [k.trim(), Object.fromEntries(f.filter(([p]) => p.trim()))]));
  return out;
}

// ---------------------------------------------------------------- new items ("add" clones)

export interface ItemDef { guid: string; name: string; base: string; displayName: string; description: string; rolledLoot: boolean; fields: [string, Json][]; rest: Obj }
export function itemDefsOf(section: Json | undefined): ItemDef[] {
  const add = isObj(section) && Array.isArray(section.add) ? section.add : [];
  return add.filter(isObj).map((d) => {
    const { guid, name, base, displayName, description, rolledLoot, fields, ...rest } = d;
    return { guid: String(guid ?? ""), name: String(name ?? ""), base: String(base ?? ""), displayName: typeof displayName === "string" ? displayName : "",
      description: typeof description === "string" ? description : "", rolledLoot: rolledLoot === true, fields: isObj(fields) ? Object.entries(fields) : [], rest };
  });
}
export function itemDefValue(d: ItemDef): Obj {
  const out: Obj = { guid: d.guid, name: d.name.trim(), base: d.base.trim() };
  if (d.displayName.trim()) out.displayName = d.displayName;
  if (d.description.trim()) out.description = d.description;
  Object.assign(out, d.rest);
  const f = d.fields.filter(([k]) => k.trim());
  if (f.length) out.fields = Object.fromEntries(f);
  if (d.rolledLoot) out.rolledLoot = true;
  return out;
}
export const NAME_RE = /^[A-Za-z0-9_.-]{1,200}$/;
export const newGuid = (): string => (typeof crypto !== "undefined" && "randomUUID" in crypto ? crypto.randomUUID()
  : "xxxxxxxx-xxxx-4xxx-8xxx-xxxxxxxxxxxx".replace(/x/g, () => Math.floor(Math.random() * 16).toString(16)));
