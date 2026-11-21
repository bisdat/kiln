// When Crucible looks at the clipboard (pure; tested in unit/crucible-clipboard.test.ts). Never just because the
// launcher started or updated: at start only when crucible_resume says a pack page's download left a pack waiting; then
// when the player opens "Got a code?" or presses its Paste. The read itself is the backend's (crucible_clipboard), never
// navigator.clipboard (WebView2 would ask the player for clipboard access). Nothing here logs or keeps the text.
import type { CrucibleResume } from "../api/commands";
import { codeOf, crucibleTarget } from "./code";

export type ClipboardRead = () => Promise<string | null>;

/** The pack the start opens: the code the download carried, else the clipboard's pack when one waits; null otherwise
 *  (the clipboard is then not read at all). */
export async function resumeTarget(r: CrucibleResume, read: ClipboardRead): Promise<string | null> {
  if (r.code) return r.code;
  if (!r.packWaiting) return null;
  return crucibleTarget(await safe(read));
}

/** What the code field shows for this text: the code (of a code or share link), a link without one as it is; null for
 *  text that is not a Crucible code or link (the field's own check). */
export function codePrefill(text: string | null | undefined): string | null {
  const x = crucibleTarget(text);
  return x ? codeOf(x) ?? x : null;
}

/** "Got a code?" opening, or its Paste: the clipboard's code for the field, or null. */
export async function clipboardPrefill(read: ClipboardRead): Promise<string | null> {
  return codePrefill(await safe(read));
}

async function safe(read: ClipboardRead): Promise<string | null> {
  try { return await read(); } catch { return null; }
}
