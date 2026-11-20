// Crucible's small pure helpers: pack codes, the targets a player pastes, the default cover of a pack, change counts.
// Tested in unit/crucible.test.ts.
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
