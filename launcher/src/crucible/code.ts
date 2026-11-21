// Crucible's small pure helpers: pack codes, the targets a player pastes, the default cover of a pack, change counts,
// the name an installed pack gets, what a share sends, and the check line of the preview. Tested in unit/crucible.test.ts.
import type { CrucibleChecks, LibraryEntry, ModKind, Profile } from "../api/commands";
import arcane from "../assets/crucible/cover-arcane.webp";
import builder from "../assets/crucible/cover-builder.webp";
import forge from "../assets/crucible/cover-forge.webp";
import frost from "../assets/crucible/cover-frost.webp";
import shroud from "../assets/crucible/cover-shroud.webp";
import wilds from "../assets/crucible/cover-wilds.webp";

/** The code alphabet: digits and capitals without the look-alikes (0 O 1 I L). */
export const CODE_ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
const CODE_RE = new RegExp("^[" + CODE_ALPHABET + "]{8}$");

/** "h7qk 3mzp", "H7QK-3MZP", "h7qk3mzp" -> "H7QK-3MZP"; null when it is not 8 symbols of the alphabet. */
export function normalizeCode(text: string): string | null {
  const s = text.trim().toUpperCase().replace(/[\s\-_.]/g, "");
  return CODE_RE.test(s) ? s.slice(0, 4) + "-" + s.slice(4) : null;
}

/** The code typed so far, as the field shows it: capitals, the dash after four, nothing outside the alphabet. */
export function codeInput(text: string): string {
  const s = text.toUpperCase().replace(/[^0-9A-Z]/g, "").split("").filter((c) => CODE_ALPHABET.includes(c)).join("").slice(0, 8);
  return s.length > 4 ? s.slice(0, 4) + "-" + s.slice(4) : s;
}

const LINK_RE = /^(?:https:\/\/)?frostdev\.io\/kiln\/crucible\/(?:p\/[0-9A-Za-z-]{8,9}|[a-z0-9-]{3,})\/?$/i;
const KILN_RE = /^kiln:\/\/crucible\/(?:pack\/[a-z0-9-]{3,}|[0-9A-Za-z-]{8,9})\/?$/i;

/** What crucible_open takes from what a player pasted or a link brought: a code (normalized), a share link, a kiln://
 *  link, or a version id; null for anything else (a clipboard holding other text). */
export function crucibleTarget(text: string | null | undefined): string | null {
  const s = (text ?? "").trim();
  if (!s || s.length > 200) return null;
  const code = normalizeCode(s);
  if (code) return code;
  if (/\s/.test(s)) return null;
  if (LINK_RE.test(s)) return s.startsWith("http") ? s : "https://" + s;
  if (KILN_RE.test(s)) return s;
  if (/^pv_[A-Za-z0-9]{2,64}$/.test(s)) return s;
  return null;
}

/** The code in a share link or kiln:// link, if it carries one ("…/p/H7QK-3MZP"). */
export function codeOf(target: string): string | null {
  const m = /(?:\/p\/|kiln:\/\/crucible\/)([0-9A-Za-z-]{8,9})\/?$/i.exec(target);
  return m ? normalizeCode(m[1]) : normalizeCode(target);
}

const COVERS = [forge, frost, shroud, wilds, arcane, builder];
/** A pack's default cover, the same for the same pack id everywhere (FNV-1a over the id). */
export function coverIndex(id: string): number {
  let h = 0x811c9dc5;
  for (const c of id) h = Math.imul(h ^ c.charCodeAt(0), 16777619) >>> 0;
  return h % COVERS.length;
}
export function coverFor(id: string, icon?: string | null): string {
  return icon && /^(https:|data:image\/)/.test(icon) ? icon : COVERS[coverIndex(id)];
}

export interface ChangeCounts { added: number; updated: number; removed: number }
export const countsOf = (c: { added: unknown[]; updated: unknown[]; removed: unknown[] } | null | undefined): ChangeCounts =>
  ({ added: c?.added.length ?? 0, updated: c?.updated.length ?? 0, removed: c?.removed.length ?? 0 });
export const anyChange = (c: ChangeCounts) => c.added + c.updated + c.removed > 0;

// ---------------------------------------------------------------- an installed pack's name

/** `name`, or "name (2)", "name (3)"... when a profile has it already (trimmed, any case), at most 128 characters:
 *  kiln_manage::profile::unique_name's rule (the backend applies it again, so two profiles never share a name). */
export function uniqueName(name: string, taken: Iterable<string>): string {
  const names = new Set([...taken].map((n) => n.trim().toLowerCase()));
  const fit = (suffix: string) => Array.from(name.trim()).slice(0, 128 - suffix.length).join("").trimEnd() + suffix;
  const first = fit("");
  if (!first || !names.has(first.toLowerCase())) return first;
  for (let n = 2; n < 10_000; n++) { const c = fit(" (" + n + ")"); if (!names.has(c.toLowerCase())) return c; }
  return first;
}

// ---------------------------------------------------------------- what a share sends

/** The player layer's id prefix (kiln_manage::modpack::pack::PLAYER_LAYER_PREFIX): a share never sends it. */
export const PLAYER_LAYER_PREFIX = "local.";
export interface ShareEstimate { mods: number; bytes: number }
/** What the first share of a profile sends at most: every mod the pack lists (switched-off ones go as optional, off)
 *  and their Library files' size. The archives are compressed and Crucible keeps files it has, so the upload is this or
 *  less. Null until the profile and the Library are known. */
export function shareEstimate(profile: Profile | null | undefined, library: LibraryEntry[] | null | undefined): ShareEstimate | null {
  if (!profile || !library) return null;
  let mods = 0, bytes = 0;
  for (const [kind, list] of Object.entries(profile.mods) as [ModKind, Profile["mods"][ModKind]][]) {
    for (const m of list ?? []) {
      if (kind === "kiln" && m.id.startsWith(PLAYER_LAYER_PREFIX)) continue;
      mods++;
      const e = library.find((x) => x.kind === kind && x.id === m.id);
      const v = e?.versions.find((x) => x.hash === m.hash) ?? e?.versions.find((x) => x.version === m.version);
      bytes += v?.size ?? 0;
    }
  }
  return { mods, bytes };
}

// ---------------------------------------------------------------- the preview's check line

/** "Checks: passed" / "N things to look at" / "N problems" / "still running" (with a refresh) / "could not run": the
 *  line the preview builds from Crucible's report (`version.checks`). The quick index checks come at once, the full
 *  check a few minutes after the upload: while it runs, problems the quick checks found still show. Null: no report
 *  (an older Crucible). */
export type ChecksLine = { kind: "passed" | "warnings" | "problems" | "pending" | "failed"; count: number; messages: string[] };
export function checksLine(c: CrucibleChecks | null | undefined): ChecksLine | null {
  if (!c || typeof c !== "object" || !c.state) return null;
  const errors = c.errors ?? [], warnings = c.warnings ?? [];
  if (errors.length) return { kind: "problems", count: errors.length, messages: errors.slice(0, 5) };
  if (c.state === "queued" || c.state === "running" || c.state === "waiting") return { kind: "pending", count: 0, messages: [] };
  if (c.state === "failed") return { kind: "failed", count: 0, messages: [] };
  if (warnings.length) return { kind: "warnings", count: warnings.length, messages: warnings.slice(0, 5) };
  return { kind: "passed", count: 0, messages: [] };
}
