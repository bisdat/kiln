// (29.18, inbox 85) Kiln's files in the game folder as Runtime shows them: the receipt files that are gone (an
// antivirus removed or quarantined one, or a hand delete) apart from those that are there with other bytes.
// install::status lists both in `modified` and the gone ones again in `missingFiles`. A missing file is put back by
// installing the same bundle again (Runtime's Repair); a changed one keeps today's banner without a button.
import type { InstallState, Platform } from "../api/commands";

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
