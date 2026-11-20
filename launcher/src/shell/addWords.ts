// (29.18, inbox 90) What an add's toast calls each mod it switched on: "Added" for a mod new in the profile, "Updated"
// for another version of a mod the profile already had (a newer file of it dropped on the window, or added on the
// Deck), with both versions where the toast has room ("Updated Night Owl: 1.2.0 → 1.3.0"). The same version again (a
// re-add, or a mod switched back on) says what it said before. The report's `changes` hold what the profile had
// (kiln_manage::quick::merge: `before`), so this needs nothing new from the backend.
import type { AddedMod, AddReport } from "../api/commands";
import { t } from "../i18n";
import { names } from "./importResult";

export interface UpdatedMod {
  mod: AddedMod;
  /** The version the profile had. */
  from: string;
}

export interface AddSplit {
  /** New in the profile, or the same version switched on. */
  added: AddedMod[];
  /** Another version of a mod the profile had. */
  updated: UpdatedMod[];
}

/** `r.added` split by what each one was in the profile before. */
export function splitAdded(r: Pick<AddReport, "added" | "changes">): AddSplit {
  const out: AddSplit = { added: [], updated: [] };
  for (const m of r.added) {
    const c = r.changes.find((x) => x.kind === m.kind && x.after.id === m.id && x.after.version === m.version && x.after.hash === m.hash);
    if (c?.before && c.before.id === m.id && c.before.version !== m.version) out.updated.push({ mod: m, from: c.before.version });
    else out.added.push(m);
  }
  return out;
}

/** Versions short enough for a toast's title ("1.2.0", "2.0.0-beta.3"), not a build id. */
const fits = (v: string) => v.length <= 20;

/** The desktop toast's title: "Added Night Owl", "Updated Night Owl: 1.2.0 → 1.3.0", "Updated Night Owl and Dawn
 *  Lamp", "Added Night Owl, updated Dawn Lamp". */
export function addTitle(r: Pick<AddReport, "added" | "changes">): string {
  const s = splitAdded(r);
  if (!s.updated.length) return t("shell.catalogInstall.added", { name: names(r.added.map((m) => m.name)) });
  const up = s.updated.map((u) => u.mod.name);
  if (s.added.length) return t("shell.drop.addedAndUpdated", { added: names(s.added.map((m) => m.name)), updated: names(up) });
  const [one] = s.updated;
  if (s.updated.length === 1 && fits(one.from) && fits(one.mod.version))
    return t("shell.drop.updatedFromTo", { name: one.mod.name, from: one.from, to: one.mod.version });
  return t("shell.catalogInstall.updated", { name: names(up) });
}

/** The Deck toast's title, in the Deck's words ("Added 3 mods" stays a count). */
export function deckAddTitle(r: Pick<AddReport, "added" | "changes">): string {
  const s = splitAdded(r);
  if (r.added.length === 1) {
    const [one] = s.updated;
    if (!one) return t("deck.addMods.addedOne", { name: r.added[0].name });
    return fits(one.from) && fits(one.mod.version)
      ? t("deck.addMods.updatedOne", { name: one.mod.name, from: one.from, to: one.mod.version })
      : t("deck.addMods.updatedName", { name: one.mod.name });
  }
  if (!s.updated.length) return t("deck.addMods.addedMany", { count: r.added.length });
  if (!s.added.length) return t("deck.addMods.updatedMany", { count: s.updated.length });
  return t("deck.addMods.addedAndUpdated", { added: s.added.length, updated: s.updated.length });
}
