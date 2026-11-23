// Format 1: schema/state-registry.schema.json, native/services/content/registry/report.cpp.
// Only a bounded, display-only projection crosses into React. Unknown members are ignored.
import published from "../../../schema/registry/ids.lock.json";
export const REPORT_LIMITS = { bytes: 8 * 1024 * 1024, rows: 500, writers: 64, text: 2048, depth: 48 } as const;
const kilnFields = new Map(Object.entries(published.registries).map(([name, spec]) => [name, new Set(Object.keys(spec.fields))]));
function isKilnField(registry: string, field: string): boolean {
  const normalized = field.replace(/\[(?:\d+|\*)\]/g, "[]");
  const fields = kilnFields.get(registry);
  return !!fields && (fields.has(normalized) || fields.has(normalized.replace(/\[\]$/, "")));
}
type ObjectRow = Record<string, unknown>;
export interface RegistryWriter { owner: string; kind: string; stage: string; operation: string; value: string; certainty: string }
export interface RegistryField {
  registry: string; id: string; field: string; applies: string; baseOrigin: string; base: string;
  result: string; winner: string; writers: RegistryWriter[]; writersOmitted: number; formulaInputs: string;
}
export interface RegistryConflict { field: string; kind: string; winner: string; loser: string; certainty: string; text: string }
export interface RegistryRefusal { owner: string; field: string; stage: string; reason: string }
export interface RegistryReport {
  written: string; generation: number; phase: string; baseOrigin: string; baseWriter: string; verified: boolean;
  fields: RegistryField[]; conflicts: RegistryConflict[]; refused: RegistryRefusal[];
  entries: { kind: string; registry: string; id: string }[]; omitted: number; empty: boolean;
}
function object(x: unknown): ObjectRow {
  if (x === null || typeof x !== "object" || Array.isArray(x)) throw new Error("Invalid registry report object.");
  return x as ObjectRow;
}
function array(x: unknown): unknown[] {
  if (!Array.isArray(x)) throw new Error("Invalid registry report list.");
  return x;
}
function text(x: unknown): string {
  if (typeof x !== "string") throw new Error("Invalid registry report text.");
  // Raw addresses are not Kiln identifiers. Also remove terminal/bidi controls that could disguise an owner.
  const s = x.slice(0, REPORT_LIMITS.text).replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f‪-‮⁦-⁩]/g, "")
    .replace(/\b(?:RVA|offset)\s*[:=]?\s*(?:0x[\da-f]+|\d+)\b|\b0x[\da-f]{6,}\b/gi, "[native detail hidden]");
  return s + (x.length > REPORT_LIMITS.text ? "…" : "");
}
function count(x: unknown): number {
  if (!Number.isSafeInteger(x) || (x as number) < 0) throw new Error("Invalid registry report count.");
  return x as number;
}
const omitted = (x: unknown): number => x === undefined ? 0 : count(x);
const optional = (x: unknown): string => x === undefined ? "" : text(x);
const owner = (x: unknown): string => x === null ? "Base" : text(x);
function certainty(x: unknown): string {
  return x === "certain" ? "Certain" : x === "named" ? "Named from journal" : x === "bestEffort" ? "Best-effort drift" : "Unknown";
}

// Values may contain lists/records, but never recursively stringify an untrusted tree wholesale.
function value(x: unknown, leftOut: unknown = 0): string {
  let budget = 128;
  function bounded(v: unknown, depth: number): unknown {
    if (--budget < 0 || depth > 4) return "… omitted";
    if (v === null || typeof v === "boolean") return v;
    if (typeof v === "number") return Number.isFinite(v) ? v : "Invalid number";
    if (typeof v === "string") return text(v);
    if (Array.isArray(v)) {
      const rows = v.slice(0, 16).map(a => bounded(a, depth + 1));
      if (v.length > 16) rows.push(`… ${v.length - 16} omitted`);
      return rows;
    }
    const o = object(v);
    const keys = Object.keys(o).filter(k => !/^(raw|offset|rva|address|function|gameFunction)$/i.test(k));
    const rows: ObjectRow = Object.create(null);
    for (const k of keys.slice(0, 16)) rows[text(k)] = bounded(o[k], depth + 1);
    if (keys.length > 16) rows["…"] = `${keys.length - 16} omitted`;
    return rows;
  }
  const result = text(JSON.stringify(bounded(x, 0)));
  const n = omitted(leftOut);
  return result + (n ? ` (${n} omitted)` : "");
}

function checkEnvelope(input: string) {
  if (input.length > REPORT_LIMITS.bytes || new TextEncoder().encode(input).length > REPORT_LIMITS.bytes) throw new Error("Registry report exceeds the 8 MiB limit.");
  let depth = 0, quoted = false, escape = false, tokens = 0;
  for (const c of input) {
    if (quoted) {
      if (escape) escape = false;
      else if (c === "\\") escape = true;
      else if (c === '"') quoted = false;
    } else if (c === '"') quoted = true;
    else if (c === "{" || c === "[") {
      if (++depth > REPORT_LIMITS.depth) throw new Error("Registry report nesting limit exceeded.");
      tokens++;
    } else if (c === "}" || c === "]") depth--;
    else if (c === "," || c === ":") tokens++;
    if (tokens > 200_000) throw new Error("Registry report structure limit exceeded.");
  }
}

export function parseRegistryReport(input: string): RegistryReport {
  checkEnvelope(input);
  let parsed: unknown;
  try { parsed = JSON.parse(input); } catch { throw new Error("Registry report is not valid JSON."); }
  const r = object(parsed);
  if (r.format !== 1) throw new Error("Unsupported registry report format (expected 1).");
  const written = text(r.written);
  if (!/^\d{4}-\d\d-\d\dT/.test(written) || !Number.isFinite(Date.parse(written))) throw new Error("Invalid registry report timestamp.");
  const base = object(r.base), regmap = object(r.regmap), totals = object(r.counts), truncated = object(r.truncated);
  if (typeof regmap.verified !== "boolean") throw new Error("Invalid registry mapping status.");
  const fields = array(r.fields), conflicts = array(r.conflicts), refused = array(r.refused), added = array(r.added), removed = array(r.removed);
  let left = 0, total = 0;
  for (const [key, rows] of Object.entries({ fields, conflicts, refused, added, removed })) {
    total += Math.max(count(totals[key]), rows.length);
    left += count(truncated[key]) + Math.max(0, rows.length - REPORT_LIMITS.rows);
  }
  const fieldRows = fields.slice(0, REPORT_LIMITS.rows).map(x => {
    const f = object(x), b = object(f.base), chain = array(f.writers);
    const skip = Math.max(0, chain.length - REPORT_LIMITS.writers);
    const raw = f.raw === true || !isKilnField(text(f.registry), text(f.field));
    return {
      registry: text(f.registry), id: text(f.id), field: raw ? "Advanced field (details hidden)" : text(f.field), applies: text(f.applies),
      baseOrigin: text(b.origin), base: value(b.value, b.entriesLeftOut), result: value(f.result, f.resultEntriesLeftOut), winner: owner(f.winner),
      formulaInputs: f.formulaInputs === undefined ? "" : value(f.formulaInputs),
      writersOmitted: omitted(f.writersLeftOut) + skip,
      writers: chain.slice(skip).map(x => {
        const w = object(x);
        return { owner: optional(w.who) || optional(w.mod) || "Kiln reference check", kind: text(w.kind), stage: optional(w.stage),
          operation: w.list !== undefined ? value(w.list, w.listEntriesLeftOut) : w.op !== undefined ? value(w.op, w.opEntriesLeftOut) : "Reference check",
          value: value(w.value, w.valueEntriesLeftOut), certainty: certainty(w.certainty) };
      }),
    };
  });
  return {
    written, generation: count(r.generation), phase: text(r.phase), baseOrigin: text(base.origin), baseWriter: text(base.writer), verified: regmap.verified,
    fields: fieldRows,
    conflicts: conflicts.slice(0, REPORT_LIMITS.rows).map(x => {
      const c = object(x), field = text(c.field);
      const registry = text(c.registry), prefix = `${registry}[${text(c.id)}].`;
      const raw = !field.startsWith(prefix) || !isKilnField(registry, field.slice(prefix.length));
      return { field: raw ? "Advanced field (details hidden)" : field, kind: text(c.kind), winner: owner(c.winner), loser: c.loser === null ? "None" : text(c.loser),
        certainty: certainty(c.certainty), text: raw ? "Advanced field conflict; mapping details hidden." : text(c.text) };
    }),
    refused: refused.slice(0, REPORT_LIMITS.rows).map(x => {
      const f = object(x), field = optional(f.field), registry = optional(f.registry);
      const raw = !!field && !isKilnField(registry, field);
      return { owner: text(f.mod), field: raw ? "Advanced field (details hidden)" : [optional(f.registry), optional(f.id), optional(f.field)].filter(Boolean).join(" · "),
        stage: text(f.stage), reason: raw ? `${text(f.reason).startsWith("RestartRequired:") ? "RestartRequired: " : text(f.reason).startsWith("Unproven:") ? "Unproven: " : ""}Advanced field (mapping details hidden).` : text(f.reason) };
    }),
    entries: [added, removed].flatMap((rows, index) => rows.slice(0, REPORT_LIMITS.rows).map(x => {
      const e = object(x); return { kind: index === 0 ? "Added" : "Removed", registry: text(e.registry), id: text(e.id) };
    })),
    omitted: left, empty: total === 0,
  };
}

/** No PID equality: the native report has a Windows PID even under Proton. Never call a snapshot live telemetry. */
export function reportFreshness(written: string, status?: { running: boolean; started?: string | null }, now = Date.now()): string | null {
  const at = Date.parse(written);
  if (!Number.isFinite(at) || at > now + 60_000) return "The report clock cannot be verified.";
  if (!status) return "The current game process cannot be verified.";
  if (!status.running) return "The game is not running; this is its last saved snapshot.";
  if (status.started && at < Date.parse(status.started)) return "The report predates the current game start.";
  if (now - at > 86_400_000) return "This snapshot is more than 24 hours old.";
  if (!status.started || !Number.isFinite(Date.parse(status.started))) return "The current game start cannot be verified.";
  return null;
}
