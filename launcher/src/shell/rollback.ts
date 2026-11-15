// Earlier versions kept for a roll back (kiln_manage::kept, dev.29): an update keeps a mod's previous Library version
// with the settings its game-folder copy had then. "Roll back to <version>" in a mod's menu (library_rollback) puts it
// back into the loaded profile; the toast's Undo is the Undo of any profile change there (profile_revert). The Library
// card in Settings lists every kept version with its size, each one removable.
import type { QueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { GameId, LibraryEntry, LibraryVersion, ModKind, RollbackReport } from "../api/commands";
import { qk } from "../api/hooks";
import { toast } from "../ui";

/** The kept versions a mod can roll back to: never the one its profile entry uses (by version and hash: two builds can
 *  share a version), newest kept first. */
export function rollbackTargets(lib: LibraryEntry | null, entry: { version: string; hash: string } | null): LibraryVersion[] {
  return (lib?.versions ?? []).filter((v) => !!v.kept && !(entry && v.version === entry.version && v.hash === entry.hash))
    .sort((a, b) => (b.kept!.kept).localeCompare(a.kept!.kept));
}

/** The other versions "Use <version>" offers: not the one in use, not a kept one (that is "Roll back to"). */
export function otherVersions(lib: LibraryEntry | null, current: string | null | undefined): LibraryVersion[] {
  return (lib?.versions ?? []).filter((v) => !v.kept && v.version !== current);
}

export interface KeptRow { kind: ModKind; id: string; name: string; version: LibraryVersion }

/** Every kept version in the Library, newest kept first (the Library card's list). */
export function keptVersions(library: LibraryEntry[]): KeptRow[] {
  return library.flatMap((e) => e.versions.filter((v) => !!v.kept).map((version) => ({ kind: e.kind, id: e.id, name: e.name, version })))
    .sort((a, b) => b.version.kept!.kept.localeCompare(a.version.kept!.kept) || a.name.localeCompare(b.name));
}

/** What a kept version takes on disk: its files and the settings kept with it. */
export const keptSize = (v: LibraryVersion) => (v.size ?? 0) + (v.kept?.settingsSize ?? 0);

/** How many versions the Library holds that are not kept ones (the Library card's count of mods with several). */
export const liveVersions = (e: LibraryEntry) => e.versions.filter((v) => !v.kept).length;

/** The toast's second line after a roll back. */
export function rollbackLine(r: RollbackReport): string | undefined {
  const parts: string[] = [];
  if (r.settings.length) parts.push(`Its settings from before the update come back with it (${r.settings.length === 1 ? "1 file" : `${r.settings.length} files`}).`);
  if (r.kept) parts.push(`${r.from.version} is kept: roll back again to return to it.`);
  parts.push(...r.notices);
  return parts.length ? parts.join(" ") : undefined;
}

const refresh = (qc: QueryClient, profileId: string) => Promise.all([
  qc.invalidateQueries({ queryKey: ["library_list"] }),
  qc.invalidateQueries({ queryKey: qk.profile(profileId) }),
  qc.invalidateQueries({ queryKey: qk.profiles }),
  qc.invalidateQueries({ queryKey: ["profile_resolve"] }),
  qc.invalidateQueries({ queryKey: ["found_scan"] }),
]);

/** "Roll back to <version>": edits still waiting for the profile are saved first (`flush`), then the backend swaps the
 *  entry; the toast offers Undo (profile_revert with the roll back's changes). True when done (an error is toasted). */
export async function rollBack(qc: QueryClient, gameId: GameId, profileId: string, kind: ModKind, id: string, name: string,
                               to: LibraryVersion, flush: () => Promise<boolean>): Promise<boolean> {
  try {
    if (!(await flush())) return false;
    const r = await api.libraryRollback(gameId, profileId, kind, id, { version: to.version, hash: to.hash });
    await refresh(qc, profileId);
    toast.ok(`${r.name} is back on ${r.to.version}`, rollbackLine(r), { actions: [{ label: "Undo", onClick: () => void undo(qc, r) }] });
    return true;
  } catch (e) {
    toast.error(e, `Could not roll back ${name}`);
    return false;
  }
}

async function undo(qc: QueryClient, r: RollbackReport) {
  try {
    const p = await api.profileRevert(r.profileId, r.changes);
    await refresh(qc, r.profileId);
    const m = (p.mods[r.kind] ?? []).find((x) => x.id === r.id);
    if (m && m.version === r.from.version && m.hash === r.from.hash) toast.ok(`${r.name} is on ${r.from.version} again`);
    else toast.info("Undo kept later changes", `${r.name} was changed since the roll back.`);
  } catch (e) { toast.error(e, "Undo failed"); }
}
