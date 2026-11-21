// The folder a file dialog opens in (dev.30; the choice: src-tauri/src/dialog_folder.rs). A dialog given no folder
// opens where the process runs, and an AppImage runs the launcher inside its own read-only mount
// (/tmp/.mount_kiln-.../usr): "Save report..." offered a folder that takes no file, or one that is gone once the AppImage
// exits. So every Save and Open dialog asks the backend first: the folder this kind of dialog was last used in while it
// still exists, else Downloads, else home; never the working directory, $APPDIR or an AppImage mount. The folder a
// dialog ended in is kept per kind (settings.json, dialogFolders).
import * as api from "../api/commands";

/** Each kind keeps its own last folder. */
export type DialogKind =
  | "supportReport" | "addMods" | "replaceDll" | "packSave" | "studioMods" | "studioPackSave" | "editorSave"
  | "dataFolder" | "runtimeBundle" | "files" | "folder" | "file";

/** The `defaultPath` for a dialog of `kind`: the backend's folder (with `fileName`: "<folder>/<fileName>", a Save
 *  dialog's suggestion). `preferred`: a path the caller has (a path box's value), used when it exists. When the backend
 *  cannot answer (a plain browser, an older backend), `preferred` or `fileName` as before. */
export async function dialogDefault(kind: DialogKind, opts: { fileName?: string; preferred?: string } = {},
  ask: typeof api.appDialogDefault = api.appDialogDefault): Promise<string | undefined> {
  try {
    const p = await ask(kind, opts.fileName ?? null, opts.preferred || null);
    if (p) return p;
  } catch { /* the dialog's own choice */ }
  return opts.preferred || opts.fileName || undefined;
}

/** Keeps the folder a dialog of `kind` ended in: `path` itself for a folder dialog, else its folder. Never fails. */
export function rememberDialog(kind: DialogKind, path: string | null | undefined, folder: boolean): void {
  if (!path || !api.isTauri()) return;
  api.appDialogRemember(kind, path, folder).catch(() => undefined);
}

/** Native Save dialog, opening at "<the kind's folder>/<fileName>"; null when canceled or not in Tauri. */
export async function saveDialog(o: { kind: DialogKind; fileName: string; title?: string; filters?: { name: string; extensions: string[] }[] }): Promise<string | null> {
  if (!api.isTauri()) return null;
  const { save } = await import("@tauri-apps/plugin-dialog");
  const r = await save({ title: o.title, defaultPath: await dialogDefault(o.kind, { fileName: o.fileName }), filters: o.filters });
  rememberDialog(o.kind, r, false);
  return r;
}
