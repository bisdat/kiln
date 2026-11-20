// (29.18, inbox 85) Kiln's files in the game folder as Runtime shows them: the receipt files that are gone (an
// antivirus removed or quarantined one, or a hand delete) apart from those that are there with other bytes.
// install::status lists both in `modified` and the gone ones again in `missingFiles`. A missing file is put back by
// installing the same bundle again (Runtime's Repair); a changed one keeps today's banner without a button.
import type { InstallState, KilnError, Platform } from "../api/commands";

/** The player docs' Antivirus section (docs/kiln/launcher/app.md), linked from Runtime's banner. */
export const ANTIVIRUS_DOCS = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/launcher/app.md#antivirus";

export interface FileProblems {
  /** Receipt files not in the game folder. */
  missing: string[];
  /** Receipt files there with other bytes than the install wrote. */
  changed: string[];
  /** One of the missing files is a DLL: what antivirus tools remove. */
  dll: boolean;
}

/** Splits the status's changed files into missing and changed. An older backend sends no `missingFiles`: everything
 *  is "changed" then, as before. */
export function fileProblems(st: Pick<InstallState, "modified" | "missingFiles"> | null | undefined): FileProblems {
  const missing = st?.missingFiles ?? [];
  const changed = (st?.modified ?? []).filter((f) => !missing.includes(f));
  return { missing, changed, dll: missing.some((f) => /\.dll$/i.test(f)) };
}

/** A receipt path as the player's system writes it: `kiln\plugins\kiln-greeneye.dll` on Windows. */
export function gamePath(rel: string, platform: Platform | undefined): string {
  return platform === "windows" ? rel.replace(/\//g, "\\") : rel;
}

/** The first three paths, then " …" (a banner line stays short). */
export function fewPaths(files: string[], platform: Platform | undefined): string {
  return files.slice(0, 3).map((f) => gamePath(f, platform)).join(", ") + (files.length > 3 ? " …" : "");
}

/** (29.18, inbox 92) A file Windows would not let Kiln write during an install, update, Repair or rollback (access
 *  denied, os error 5, or a sharing violation, os error 32, on a DLL or exe): an antivirus or another program may be
 *  blocking it. kiln_manage::blocked puts it into the error's `detail.blocked`; the error's message keeps the raw words. */
export interface BlockedFile {
  /** Game-relative, "/" separators: `kiln/plugins/kiln-greeneye.dll`. */
  path: string;
  /** 5 (access denied) or 32 (sharing violation). */
  osError: number;
}

/** The file an error says Windows refused, or null (any other error, an older backend). */
export function blockedFile(e: Pick<KilnError, "detail"> | null | undefined): BlockedFile | null {
  const d = e?.detail;
  const b = d && typeof d === "object" && !Array.isArray(d) ? (d as { blocked?: unknown }).blocked : undefined;
  if (!b || typeof b !== "object") return null;
  const { path, osError } = b as Partial<BlockedFile>;
  return typeof path === "string" && path !== "" && typeof osError === "number" ? { path, osError } : null;
}
