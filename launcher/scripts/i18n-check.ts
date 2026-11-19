// Checks the launcher's locale files against en.json (docs/kiln/translating.md):
//   - a key en.json does not have (a typo, or a text the launcher no longer shows),
//   - a value that is not text,
//   - placeholders that differ from en.json's ({0}, {name}, {count:N0}: the names must match; a plural form may leave
//     out {count}), and number formats that differ ({count:N0} where English has {count}, or the other way round),
//   - what a translation is never to bring (it is a player's file, read as data): an HTML tag or entity, a Markdown
//     link, a link or URL, an invisible or control character, a line break, where English has none at that key; an
//     empty text; a text far longer than English's; a confirm word (common.confirmWord.*) that is not one short word,
// and, with --source, that every t("key") in launcher/src (the save editor aside) is in en.json.
//
//   bun scripts/i18n-check.ts                       every src/i18n/*.json, and the source
//   bun scripts/i18n-check.ts ~/zh-CN.json          one file (a translation sent by mail, a PR's file)
//
// Exit 1 when anything is wrong. Keys a translation does not have yet are counted, not wrong (they show in English).
import { readdirSync, readFileSync } from "node:fs";
import { basename, join, relative } from "node:path";
import { parse, type ParseError } from "jsonc-parser";
import { flatten, placeholders, placeholderTokens, pluralBase, PLURAL_FORMS, type Table } from "../src/i18n/format";

export interface Problem {
  key: string;
  kind: "unknown-key" | "not-text" | "placeholders" | "format" | "markup" | "control" | "empty" | "length" | "confirm-word" | "not-json";
  message: string;
}

/** What a translation must not have more of than English at the same key: the launcher shows texts as React text (and
 * the admin map escapes them for Leaflet), so none of these would work as markup, but a translator never needs them. */
const MARKUP: readonly [string, RegExp][] = [
  ["an HTML tag", /<\s*\/?\s*[A-Za-z!?]/g],
  ["an HTML entity", /&(?:#\d+|#x[0-9A-Fa-f]+|[A-Za-z][A-Za-z0-9]*);/g],
  ["a Markdown link", /\]\s*\(/g],
  ["a link", /\b[A-Za-z][A-Za-z0-9+.-]*:\/\/|\b(?:javascript|data|vbscript|file|mailto):|\bwww\./gi],
];
/** Invisible and control characters (line breaks, tabs, bidi overrides, zero-width ones, private use, unassigned). */
const CONTROL = /[\p{Cc}\p{Cf}\p{Co}\p{Cs}\p{Cn}\p{Zl}\p{Zp}]/gu;
// a URL as written in English (ASCII), without the sentence's last punctuation
const URL = /\b[A-Za-z][A-Za-z0-9+.-]*:\/\/[^\s"'<>()\u0080-\uffff]*/g;
const urls = (s: string) => (s.match(URL) ?? []).map((u) => u.replace(/[.,;:!?]+$/, ""));
const count = (s: string, re: RegExp) => (s.match(re) ?? []).length;
const formats = (s: string) => placeholderTokens(s).filter((p) => p.includes(":")).sort().join(" ");

/** The data checks of one text against English's text(s) for its key. */
function dataProblems(key: string, text: string, english: string[]): Problem[] {
  const out: Problem[] = [];
  for (const [what, re] of MARKUP)
    if (count(text, re) > Math.max(...english.map((e) => count(e, re)))) out.push({ key, kind: "markup", message: `${what}, which English has not here` });
  const links = new Set(english.flatMap(urls));
  for (const u of urls(text)) if (!links.has(u)) out.push({ key, kind: "markup", message: `the link ${u}, which English has not here` });
  for (const c of new Set(text.match(CONTROL) ?? []))
    if (!english.some((e) => e.includes(c)))
      out.push({ key, kind: "control", message: `U+${c.codePointAt(0)!.toString(16).toUpperCase().padStart(4, "0")} (an invisible or control character), which English has not here` });
  if (text.trim() === "" && english.some((e) => e.trim() !== "")) out.push({ key, kind: "empty", message: "empty (a text the translation lacks shows in English: leave the key out instead)" });
  const longest = Math.max(...english.map((e) => e.length));
  if (text.length > 4 * longest + 80) out.push({ key, kind: "length", message: `${text.length} characters, English ${longest}` });
  if (/(^|\.)confirmWord\./.test(key) && !/^[^\s\p{C}]{1,12}$/u.test(text))
    out.push({ key, kind: "confirm-word", message: "a word the player types to confirm: one short word, no spaces" });
  return out;
}

/** en.json's text(s) for a key a locale has: for a plural form, all of English's forms of its base (a language's
 * `one` may be more than 1: Russian's is also 21, 31, ...; French and Brazilian Portuguese's also 0, so its `.one` may
 * need the `{count}` English's `.one` leaves out); else the key itself. Null: en.json lacks it. */
function englishFor(en: Table, key: string): string[] | null {
  const base = pluralBase(key);
  if (base !== key) {
    const forms = PLURAL_FORMS.map((f) => en[`${base}.${f}`]).filter((x): x is string => x !== undefined);
    if (forms.length) return forms;
  }
  return en[key] !== undefined ? [en[key]] : null;
}

/** What is wrong in one locale file's object, compared with en.json's. */
export function checkLocale(enTable: Table, file: unknown): Problem[] {
  const out: Problem[] = [];
  if (file === null || typeof file !== "object" || Array.isArray(file)) {
    return [{ key: "", kind: "not-json", message: "the file must hold one { ... } object" }];
  }
  const bad: string[] = [];
  const table = flatten(file, "", {}, bad);
  for (const key of bad) out.push({ key, kind: "not-text", message: "values are text in quotes" });
  for (const [key, text] of Object.entries(table)) {
    const english = englishFor(enTable, key);
    if (!english) {
      out.push({ key, kind: "unknown-key", message: "en.json has no such key" });
      continue;
    }
    // allowed: what any English form of the key has; needed: what English's own text at this key has (all forms' for a
    // plural form English does not have, e.g. Russian's .few)
    const allowed = new Set(english.flatMap(placeholders));
    const want = new Set(enTable[key] !== undefined ? placeholders(enTable[key]) : allowed);
    const have = new Set(placeholders(text));
    const plural = pluralBase(key) !== key;
    const extra = [...have].filter((p) => !allowed.has(p));
    const missing = [...want].filter((p) => !have.has(p) && !(plural && p === "count"));
    if (extra.length || missing.length) {
      const parts = [];
      if (missing.length) parts.push(`missing ${missing.map((p) => `{${p}}`).join(", ")}`);
      if (extra.length) parts.push(`not in English: ${extra.map((p) => `{${p}}`).join(", ")}`);
      out.push({ key, kind: "placeholders", message: parts.join("; ") });
    } else if (!english.some((e) => formats(e) === formats(text))) {
      // the same names: the same number formats too ({count:N0} groups digits, {count} writes the number as code does)
      out.push({ key, kind: "format", message: `number formats {${formats(text) || "none"}}, English {${formats(english[english.length - 1]) || "none"}}` });
    }
    out.push(...dataProblems(key, text, english));
  }
  return out;
}

/** en.json keys (plural forms as their base) a locale does not have: they show in English. */
export function untranslated(enTable: Table, file: unknown): string[] {
  const table = flatten(file);
  const bases = new Set(Object.keys(table).map(pluralBase));
  const seen = new Set<string>();
  const out: string[] = [];
  for (const key of Object.keys(enTable)) {
    const base = table[key] !== undefined ? key : pluralBase(key);
    if (seen.has(base)) continue;
    seen.add(base);
    if (table[key] === undefined && !bases.has(base)) out.push(base);
  }
  return out;
}

/** True when en.json has `key`, as a text or as plural forms. */
export function hasKey(enTable: Table, key: string): boolean {
  return enTable[key] !== undefined || PLURAL_FORMS.some((f) => enTable[`${key}.${f}`] !== undefined);
}

/** en.json's own shape: a key ending in a plural word (`.one`, `.other`, ...) is a plural form, so its siblings must
 * all be plural forms and include `.other` ("x.type.other" beside "x.type.kiln" would be read as a plural of x.type). */
export function pluralShapeProblems(enTable: Table): string[] {
  const kids = new Map<string, string[]>();
  for (const k of Object.keys(enTable)) {
    const i = k.lastIndexOf(".");
    if (i > 0) kids.set(k.slice(0, i), [...(kids.get(k.slice(0, i)) ?? []), k.slice(i + 1)]);
  }
  const forms = PLURAL_FORMS as readonly string[];
  const out: string[] = [];
  for (const [base, ks] of kids) {
    if (!ks.some((k) => forms.includes(k))) continue;
    const odd = ks.filter((k) => !forms.includes(k));
    if (odd.length) out.push(`${base}: ${ks.filter((k) => forms.includes(k)).join(", ")} beside ${odd.join(", ")} (a key ending in a plural word is a plural form: rename it)`);
    else if (!ks.includes("other")) out.push(`${base}: plural forms without .other`);
    if (enTable[base] !== undefined) out.push(`${base}: both a text and plural forms`);
  }
  return out;
}

/** Every literal `t("key")`, `tr("key")` (t imported as tr) and `rich("key")` in the source files under `dir` (the save editor aside): key, file, line. */
export function sourceKeys(dir: string): { key: string; file: string; line: number }[] {
  const out: { key: string; file: string; line: number }[] = [];
  const re = /(?<![\w$.])(?:t|tr|rich)\(\s*(["'`])([A-Za-z0-9_.-]+)\1/g;
  const walk = (d: string) => {
    for (const e of readdirSync(d, { withFileTypes: true })) {
      const p = join(d, e.name);
      if (e.isDirectory()) {
        if (e.name !== "editor" && e.name !== "node_modules") walk(p);
      } else if (/\.(ts|tsx)$/.test(e.name)) {
        const lines = readFileSync(p, "utf8").split("\n");
        lines.forEach((l, i) => {
          if (/^\s*(\/\/|\/?\*)/.test(l)) return; // comments
          for (const m of l.matchAll(re)) out.push({ key: m[2], file: p, line: i + 1 });
        });
      }
    }
  };
  walk(dir);
  return out;
}

/** Reads a locale file as Locale.cs does (comments and trailing commas are fine). */
export function readLocaleFile(path: string): { value: unknown; error: string | null } {
  const errors: ParseError[] = [];
  const value = parse(readFileSync(path, "utf8"), errors, { allowTrailingComma: true });
  return { value, error: errors.length ? `not JSON (error ${errors[0].error} at offset ${errors[0].offset})` : null };
}

function main(argv: string[]): number {
  const root = join(import.meta.dir, "..");
  const i18n = join(root, "src", "i18n");
  const enTable = flatten(JSON.parse(readFileSync(join(i18n, "en.json"), "utf8")));
  const given = argv.filter((a) => !a.startsWith("--"));
  const files = given.length ? given : readdirSync(i18n).filter((f) => f.endsWith(".json") && f !== "en.json").map((f) => join(i18n, f));
  let wrong = 0;
  for (const f of files) {
    const { value, error } = readLocaleFile(f);
    const problems: Problem[] = error ? [{ key: "", kind: "not-json", message: error }] : checkLocale(enTable, value);
    const missing = error ? [] : untranslated(enTable, value);
    console.log(`${basename(f)}: ${problems.length ? `${problems.length} problem(s)` : "ok"}, ${missing.length} of ${new Set(Object.keys(enTable).map(pluralBase)).size} texts not translated yet`);
    for (const p of problems) console.log(`  ${p.key || "(file)"}: ${p.message}`);
    wrong += problems.length;
  }
  if (!given.length || argv.includes("--source")) {
    const src = join(root, "src");
    for (const p of pluralShapeProblems(enTable)) console.log(`  en.json ${p}`);
    wrong += pluralShapeProblems(enTable).length;
    const unknown = sourceKeys(src).filter((k) => !hasKey(enTable, k.key));
    for (const k of unknown) console.log(`  ${relative(root, k.file)}:${k.line}: t("${k.key}") is not in en.json`);
    console.log(`source: ${unknown.length ? `${unknown.length} key(s) not in en.json` : "ok"}`);
    wrong += unknown.length;
  }
  return wrong ? 1 : 0;
}

if (import.meta.main) process.exit(main(process.argv.slice(2)));
