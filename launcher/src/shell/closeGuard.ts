// Closing the launcher's window while work would be lost asks first: Pack Studio's unsaved drafts, an editor's changes,
// the save editor's changes, or a save the save editor is writing right now. The desktop app only (a browser tab has
// the save editor's own beforeunload guard). The Deck's Quit has its own dialog, which names the same losses.
import { isTauri } from "../api/commands";
import { usePendingEdits } from "../editor/kiln/pending";
import { t } from "../i18n";
import { unsavedKind, useUnsaved, type UnsavedKind } from "./unsaved";

/** What closing now would throw away: a save being written ("busy") first, then [`unsavedKind`]; null: nothing. */
export type CloseLoss = "busy" | UnsavedKind;

export function closeLoss(by: Record<string, UnsavedKind>, edits: { dirty: boolean; busy?: boolean }): CloseLoss | null {
  if (edits.busy) return "busy";
  return unsavedKind(by, edits.dirty);
}

/** The question for `loss`: the save editor's character by name when it is the save editor's changes. */
export function closeQuestion(loss: CloseLoss, edits: { dirty: boolean; name: string | null }, by: Record<string, UnsavedKind>): string {
  if (loss === "busy") return t("shell.closeGuard.busy");
  if (loss === "studio") return t("shell.closeGuard.studio");
  // the save editor's own changes (and no other editor's): name the character
  if (edits.dirty && !Object.values(by).includes("editor")) {
    return edits.name != null ? t("shell.closeGuard.saveEditorNamed", { name: edits.name }) : t("shell.closeGuard.saveEditor");
  }
  return t("shell.closeGuard.editor");
}

let installed = false;

/** Asks before the window closes while closing would lose work (once per start; a no-op outside the desktop app). */
export function installCloseGuard(): void {
  if (installed || !isTauri()) return;
  installed = true;
  void (async () => {
    const [{ getCurrentWindow }, { ask }] = await Promise.all([import("@tauri-apps/api/window"), import("@tauri-apps/plugin-dialog")]);
    const win = getCurrentWindow();
    // with a listener here the window no longer closes by itself: the listener closes it (destroy) unless it is stopped
    await win.onCloseRequested(async (e) => {
      const by = useUnsaved.getState().by;
      const edits = usePendingEdits.getState();
      const loss = closeLoss(by, edits);
      if (!loss) return;
      e.preventDefault();
      // a dialog that cannot open must not trap the player in a window that never closes
      const close = await ask(closeQuestion(loss, edits, by), {
        title: t("shell.closeGuard.title"), kind: "warning",
        okLabel: t("shell.closeGuard.close"), cancelLabel: t("shell.closeGuard.stay"),
      }).catch(() => true);
      if (close) await win.destroy();
    });
  })().catch((err) => console.warn("close guard", err));
}
