// Format 1: schema/state-registry.schema.json, native/services/content/registry/report.cpp.
// Only a bounded, display-only projection crosses into React. Unknown members are ignored.
import published from "../../../schema/registry/ids.lock.json";
import { t } from "../i18n";
export const REPORT_LIMITS = { bytes: 8 * 1024 * 1024, containers: 8 * 1024 * 1024 / 16, rows: 500, writers: 64, text: 2048, depth: 48 } as const;
const kilnFields = new Map(Object.entries(published.registries).map(([name, spec]) => [name, new Set(Object.keys(spec.fields))]));
function isKilnField(registry: string, field: string): boolean {
  const normalized = field.replace(/\[(?:\d+|\*)\]/g, "[]");
  const fields = kilnFields.get(registry);
  return !!fields && (fields.has(normalized) || fields.has(normalized.replace(/\[\]$/, "")));
}
type ObjectRow = Record<string, unknown>;
// certainty is the label in the current language; certain says whether Kiln observed it (the page's tone).
export interface RegistryWriter { element: string; owner: string; kind: string; stage: string; operation: string; value: string; certainty: string; certain: boolean }
export interface RegistryField {
  /** registry|id|field|n (n: earlier rows with the same place): a React key that follows the row across pages and refetches. */
  key: string;
  registry: string; id: string; field: string; applies: string; baseOrigin: string; base: string;
  result: string; winner: string; writers: RegistryWriter[]; writersOmitted: number; formulaInputs: string;
}
export interface RegistryConflict { field: string; kind: string; winner: string; loser: string; certainty: string; certain: boolean; text: string }
export interface RegistryRefusal { owner: string; field: string; stage: string; reason: string }
export interface RegistryReport {
  written: string; generation: number; phase: string; baseOrigin: string; baseWriter: string; verified: boolean;
  fields: RegistryField[]; conflicts: RegistryConflict[]; refused: RegistryRefusal[];
  entries: { kind: string; registry: string; id: string; state: string; owner: string; source: string; reason: string }[]; omitted: number; empty: boolean;
  /** Earlier writers left out of field chains: native's truncated.writers plus this view's own 64-writer cut. */
  writersOmitted: number;
}
function object(x: unknown): ObjectRow {
  if (x === null || typeof x !== "object" || Array.isArray(x)) throw new Error(t("registry.error.malformed", { part: "object" }));
  return x as ObjectRow;
}
function array(x: unknown): unknown[] {
  if (!Array.isArray(x)) throw new Error(t("registry.error.malformed", { part: "list" }));
  return x;
}
function text(x: unknown): string {
  if (typeof x !== "string") throw new Error(t("registry.error.malformed", { part: "text" }));
  // Raw addresses are not Kiln identifiers. Also remove terminal/bidi/zero-width controls that could disguise an owner,
  // and never cut a surrogate pair in half.
  let cut = x.slice(0, REPORT_LIMITS.text);
  if (/[\ud800-\udbff]$/.test(cut)) cut = cut.slice(0, -1);
  const s = cut.replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f\u061c\u200b-\u200f\u202a-\u202e\u2060-\u2064\u2066-\u2069\ufeff]/g, "")
    .replace(/\b(?:RVA|offset)\s*[:=]?\s*(?:0x[\da-f]+|\d+)\b|\b0x[\da-f]{6,}\b/gi, "[native detail hidden]");
  return s + (x.length > REPORT_LIMITS.text ? "…" : "");
}
function count(x: unknown): number {
  if (!Number.isSafeInteger(x) || (x as number) < 0) throw new Error(t("registry.error.malformed", { part: "count" }));
  return x as number;
}
// A raw member path a mod wrote can sit inside native text attached to a Kiln field (compose.cpp: "... over a's raw path
// ItemDesc.m_stackSize", "items[x].maxStack and raw path X are one place", "raw path \"P\" is inside ...", place_text's
// "items[x].P (raw)", a FieldRef's "items[x].raw:p"). Display text never names one.
const RAW_HIDDEN = "raw path (details hidden)";
export function redactRaw(s: string): string {
  return s
    .replace(/\braw paths? "(?:[^"\\]|\\.)*"(?:\s*(?:and|,)\s*"(?:[^"\\]|\\.)*")*/g, RAW_HIDDEN)
    .replace(/\braw path (?!\(details hidden\))[^\s,;:()"]+/g, RAW_HIDDEN)
    .replace(/[^\s"]*\.raw:[^\s,;:()"]+|[^\s"]+ \(raw\)/g, RAW_HIDDEN);
}
const omitted = (x: unknown): number => x === undefined ? 0 : count(x);
const optional = (x: unknown): string => x === undefined ? "" : text(x);
const owner = (x: unknown): string => x === null ? t("registry.baseOwner") : text(x);
function certainty(x: unknown): string {
  return t(`registry.certainty.${x === "certain" || x === "named" || x === "bestEffort" ? x : "unknown"}`);
}
const ADVANCED_FIELD = () => t("registry.advancedField");
const unknownField = (field: string) => t("registry.unknownField", { field });

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
    const all = Object.keys(o), keys = all.filter(k => !/^(raw|offset|rva|address|function|gameFunction)$/i.test(k));
    const rows: ObjectRow = Object.create(null);
    for (const k of keys.slice(0, 16)) rows[text(k)] = bounded(o[k], depth + 1);
    if (keys.length > 16) rows["…"] = `${keys.length - 16} omitted`;
    if (all.length > keys.length) rows["… hidden"] = `${all.length - keys.length} hidden`;
    return rows;
  }
  const result = text(JSON.stringify(bounded(x, 0)));
  const n = omitted(leftOut);
  return result + (n ? ` (${n} omitted)` : "");
}

// The native writer caps rows, not bytes (report.cpp: 20,000 fields x 64 writers x 64-entry lists can pass 8 MiB): say
// where the whole file is, rather than only that it is too large.
function checkEnvelope(input: string) {
  if (input.length > REPORT_LIMITS.bytes || new TextEncoder().encode(input).length > REPORT_LIMITS.bytes) throw new Error(t("registry.error.tooLarge"));
  // Work is bounded by the byte cap: JSON.parse is linear in it. What costs memory beyond the text are containers (each
  // object/list is an allocation), so those are capped in proportion to the byte cap: one per 16 bytes. A native report
  // spends far more than that per container (about 60 bytes for a field row), so any real report up to 8 MiB passes, while
  // a document of empty lists/objects meant to inflate memory does not. Separators are not counted (at most one per byte).
  let depth = 0, quoted = false, escape = false, containers = 0;
  for (const c of input) {
    if (quoted) {
      if (escape) escape = false;
      else if (c === "\\") escape = true;
      else if (c === '"') quoted = false;
    } else if (c === '"') quoted = true;
    else if (c === "{" || c === "[") {
      if (++depth > REPORT_LIMITS.depth) throw new Error(t("registry.error.nesting"));
      if (++containers > REPORT_LIMITS.containers) throw new Error(t("registry.error.structure"));
    } else if (c === "}" || c === "]") depth--;
  }
}

export function parseRegistryReport(input: string): RegistryReport {
  checkEnvelope(input);
  let parsed: unknown;
  try { parsed = JSON.parse(input); } catch { throw new Error(t("registry.error.notJson")); }
  const r = object(parsed);
  if (r.format !== 1) throw new Error(t("registry.error.format"));
  const written = text(r.written);
  if (!/^\d{4}-\d\d-\d\dT/.test(written) || !Number.isFinite(Date.parse(written))) throw new Error(t("registry.error.malformed", { part: "timestamp" }));
  const base = object(r.base), regmap = object(r.regmap), totals = object(r.counts), truncated = object(r.truncated);
  if (typeof regmap.verified !== "boolean") throw new Error(t("registry.error.malformed", { part: "regmap.verified" }));
  const fields = array(r.fields), conflicts = array(r.conflicts), refused = array(r.refused), added = array(r.added), removed = array(r.removed);
  let left = 0, total = 0;
  for (const [key, rows] of Object.entries({ fields, conflicts, refused, added, removed })) {
    total += Math.max(count(totals[key]), rows.length);
    left += count(truncated[key]) + Math.max(0, rows.length - REPORT_LIMITS.rows);
  }
  let launcherWriterCut = 0;
  const seen = new Map<string, number>();
  const fieldRows = fields.slice(0, REPORT_LIMITS.rows).map(x => {
    const f = object(x), b = object(f.base), chain = array(f.writers);
    const skip = Math.max(0, chain.length - REPORT_LIMITS.writers);
    launcherWriterCut += skip;
    const raw = f.raw === true, unknown = !raw && !isKilnField(text(f.registry), text(f.field));
    const field = raw ? ADVANCED_FIELD() : unknown ? unknownField(text(f.field)) : text(f.field);
    const place = `${text(f.registry)}|${text(f.id)}|${raw ? "raw" : text(f.field)}`, n = seen.get(place) ?? 0;  // never a raw path
    seen.set(place, n + 1);
    return {
      key: `${place}|${n}`,
      registry: text(f.registry), id: text(f.id), field, applies: text(f.applies),
      baseOrigin: text(b.origin), base: value(b.value, b.entriesLeftOut), result: value(f.result, f.resultEntriesLeftOut), winner: owner(f.winner),
      formulaInputs: f.formulaInputs === undefined ? "" : value(f.formulaInputs),
      writersOmitted: omitted(f.writersLeftOut) + skip,
      writers: chain.slice(skip).map(x => {
        const w = object(x);
        // an element edit names its element ("entries[3].chance", report.cpp writer_json): shown when the lock knows the
        // list and leaf, so a chain says which element an operation changed
        const element = w.element === undefined || raw ? "" : isKilnField(text(f.registry), text(w.element)) ? text(w.element) : t("registry.elementHidden");
        return { element, owner: optional(w.who) || optional(w.mod) || t("registry.referenceCheck"), kind: text(w.kind), stage: optional(w.stage),
          operation: w.list !== undefined ? value(w.list, w.listEntriesLeftOut) : w.op !== undefined ? value(w.op, w.opEntriesLeftOut) : t("registry.referenceOp"),
          value: value(w.value, w.valueEntriesLeftOut), certainty: certainty(w.certainty), certain: w.certainty === "certain" };
      }),
    };
  });
  return {
    written, generation: count(r.generation), phase: text(r.phase), baseOrigin: text(base.origin), baseWriter: text(base.writer), verified: regmap.verified,
    fields: fieldRows, writersOmitted: omitted(truncated.writers) + launcherWriterCut,
    conflicts: conflicts.slice(0, REPORT_LIMITS.rows).map(x => {
      const c = object(x), field = text(c.field);
      const registry = text(c.registry), prefix = `${registry}[${text(c.id)}].`;
      const raw = c.raw === true || !field.startsWith(prefix), unknown = !raw && !isKilnField(registry, field.slice(prefix.length));
      return { field: raw ? ADVANCED_FIELD() : unknown ? unknownField(field) : field, kind: text(c.kind), winner: owner(c.winner), loser: c.loser === null ? t("registry.noOwner") : text(c.loser),
        certainty: certainty(c.certainty), certain: c.certainty === "certain", text: raw ? t("registry.advancedConflict") : redactRaw(text(c.text)) };
    }),
    refused: refused.slice(0, REPORT_LIMITS.rows).map(x => {
      // Only a refusal the runtime marks raw (an Advanced-tier raw path, report.cpp) is redacted. A name the lock does not
      // know (a typo, or a field of a newer Kiln) is what the author wrote, and the reason is the one thing they can act on.
      const f = object(x), field = optional(f.field), registry = optional(f.registry), reason = redactRaw(text(f.reason));
      const raw = f.raw === true;
      const unknown = !raw && !!field && !isKilnField(registry, field);
      const place = [registry, optional(f.id), field].filter(Boolean).join(" · ");
      return { owner: text(f.mod), field: raw ? ADVANCED_FIELD() : unknown ? unknownField(place) : place,
        stage: text(f.stage), reason: raw ? `${reason.startsWith("RestartRequired:") ? "RestartRequired: " : reason.startsWith("Unproven:") ? "Unproven: " : ""}${t("registry.advancedReason")}` : reason };
    }),
    entries: [added, removed].flatMap((rows, index) => rows.slice(0, REPORT_LIMITS.rows).map(x => {
      const e = object(x);
      const state = typeof e.state === "string" && ["pendingWorld", "registered", "hidden", "disabled"].includes(e.state) ? e.state : "";
      return { kind: index === 0 ? t("registry.added") : t("registry.removed"), registry: text(e.registry), id: text(e.id), state,
        owner: optional(e.mod), source: optional(e.source), reason: redactRaw(optional(e.reason)) };
    })),
    omitted: left, empty: total === 0,
  };
}

/** No PID equality: the native report has a Windows PID even under Proton. Never call a snapshot live telemetry. */
export function reportFreshness(written: string, status?: { running: boolean; started?: string | null }, now = Date.now()): string | null {
  const at = Date.parse(written);
  if (!Number.isFinite(at) || at > now + 60_000) return t("registry.fresh.clock");
  if (!status) return t("registry.fresh.process");
  if (!status.running) return t("registry.fresh.stopped");
  if (status.started && at < Date.parse(status.started)) return t("registry.fresh.previousStart");
  if (now - at > 86_400_000) return t("registry.fresh.old");
  if (!status.started || !Number.isFinite(Date.parse(status.started))) return t("registry.fresh.start");
  return null;
}
