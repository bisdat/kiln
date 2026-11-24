// Local pack export only: no Crucible, Community session or network prerequisite.
import type { QueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { BuiltPack, ProfileId } from "../api/commands";
import { t } from "../i18n";
import { pickPaths, runTask, toast } from "../ui";
import { notifyLibraryChanged } from "../shell/libraryChanged";

/** The existing pack_build task performs the Library integrity and pack safety checks for both UIs. */
export function buildPackFile(profileId: ProfileId, name: string, dir: string, version = "1.0.0"): Promise<BuiltPack> {
  return runTask<BuiltPack>(() => api.packBuild({ profile: profileId, version, name }, dir), { quiet: true });
}

/** A separate, never-remembered dialog kind uses the app's safe Downloads/home choice, not a desktop picker’s
 *  last folder. No JS/working-directory fallback: without a native destination, export must not write. */
export async function defaultExportFolder(): Promise<string> {
  const dir = await api.appDialogDefault("deckPackExport", null, null);
  if (!dir) throw { code: "not_found", message: t("crucible.deck.exportNoFolder") } satisfies api.KilnError;
  return dir;
}

/** A folder the player picks (the browser mock answers itself). */
async function pickFolder(): Promise<string | null> {
  if (!api.isTauri()) return (await api.loadMock()).mockPickFolder();
  const r = await pickPaths({ directory: true, title: t("crucible.save.pick"), kind: "packSave" });
  return r?.[0] ?? null;
}

/** Save as file: the profile as a .kilnpack in a folder the player picks (pack_build). */
export async function saveAsFile(qc: QueryClient, profileId: ProfileId, name: string, version = "1.0.0"): Promise<BuiltPack | null> {
  const dir = await pickFolder();
  if (!dir) return null;
  const save = async (): Promise<BuiltPack | null> => {
    try {
      const r = await buildPackFile(profileId, name, dir, version);
      toast.ok(t("crucible.save.done", { name }), r.file, { actions: [{ label: t("crucible.save.openFolder"), onClick: () => void api.appOpenPath(dir).catch((e) => toast.error(e)) }] });
      return r;
    } catch (e) {
      if (!notifyLibraryChanged(qc, e, () => void save()) && api.toKilnError(e).code !== "canceled")
        toast.error(e, t("crucible.save.failed", { name }));
      return null;
    }
  };
  return save();
}
