// Download missing mods (crucible_repair) as the player reads it: the button's words, the line before Load it that
// names the missing mods, and how many there are. Pure (the strings only), tested in unit/crucible-repair.test.ts.
import type { PackLacking } from "../api/commands";
import { t } from "../i18n";
import { fmtBytes } from "../ui/format";

/** Mods of the installed version the instance's profile lacks (0: none, or nothing known). */
export const missingCount = (m: { repair: PackLacking | null } | undefined) => m?.repair?.missing.length ?? 0;

/** "Download missing mods (2, 13 MB)" (from Crucible), or "Add the 2 missing mods from this file". */
export function repairLabel(l: PackLacking, from: "crucible" | "file" | null = "crucible"): string {
  const count = l.missing.length;
  return from === "file" ? t("crucible.repair.fromFile", { count }) : t("crucible.repair.button", { count, size: fmtBytes(l.download) });
}

/** The line before Load it: which mods are missing, and that the game would start without them (with direct paths the
 *  activation does not refuse a missing Library folder; only Copy mode does). The pack's own layer goes by its role. */
export function missingLine(l: PackLacking): string {
  const names = l.names.filter((_, i) => !l.missing[i]?.id.startsWith("pack."));
  if (!names.length) return t("crucible.repair.missingLayer", { version: l.version });
  return t("crucible.repair.missing", { count: names.length, names: names.join(", "), version: l.version })
    + (names.length < l.missing.length ? " " + t("crucible.repair.andLayer") : "");
}
