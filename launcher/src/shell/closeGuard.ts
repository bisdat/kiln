// Closing the launcher's window while work would be lost asks first: Pack Studio's unsaved drafts, an editor's changes,
// the save editor's changes, or a save the save editor is writing right now. The desktop app only (a browser tab has
// the save editor's own beforeunload guard). The Deck's Quit has its own dialog, which names the same losses.
// The listener tells the backend at once that it took the request (app_close_seen): without that answer, or when the
// page's web process has ended, the backend closes the window by itself (src-tauri/src/webproc.rs, 0.2.0-dev.31.13).
import { appCloseSeen, isTauri } from "../api/commands";
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

/** What one close request needs: the page's state, the backend's answer, the question and the window's destroy. */
export interface CloseDeps {
  by: Record<string, UnsavedKind>;
  edits: { dirty: boolean; busy?: boolean; name: string | null };
  /** Tells the backend the page took the request (app_close_seen): `asking` the player, or closing. */
  seen: (asking: boolean) => Promise<unknown>;
  ask: (question: string) => Promise<boolean>;
  destroy: () => Promise<void>;
}

/** One close request: the backend hears first that the page took it (without that answer, or when the page's web
 *  process has ended, the backend closes the window by itself: src-tauri/src/webproc.rs); nothing to lose closes;
 *  else the question, and its Close destroys the window. */
export async function onCloseRequest(e: { preventDefault(): void }, d: CloseDeps): Promise<void> {
  const loss = closeLoss(d.by, d.edits);
  void d.seen(loss != null).catch(() => {});
  if (!loss) return;
  e.preventDefault();
  // a dialog that cannot open must not trap the player in a window that never closes
  const close = await d.ask(closeQuestion(loss, d.edits, d.by)).catch(() => true);
  if (close) await d.destroy();
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
    await win.onCloseRequested((e) => onCloseRequest(e, {
      by: useUnsaved.getState().by,
      edits: usePendingEdits.getState(),
      seen: appCloseSeen,
      ask: (question) => ask(question, {
        title: t("shell.closeGuard.title"), kind: "warning",
        okLabel: t("shell.closeGuard.close"), cancelLabel: t("shell.closeGuard.stay"),
      }),
      destroy: () => win.destroy(),
    }));
  })().catch((err) => console.warn("close guard", err));
}
