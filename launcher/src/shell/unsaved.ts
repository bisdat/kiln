// Work the launcher must not throw away by restarting itself (the launcher update's own restart, shell/UpdateNotice.tsx):
// Pack Studio's unsaved drafts, settings capture and summary, and the editors' (a mod's settings file, a server's
// settings). The save editor keeps its own tiny store (editor/kiln/pending.ts), read beside this one. A store with no
// data behind it, so the shell asks without loading those pages.
import { useEffect, useId } from "react";
import { create } from "zustand";
import { usePendingEdits } from "../editor/kiln/pending";

/** Whose work it is: Pack Studio's, or an editor's. */
export type UnsavedKind = "studio" | "editor";

export const useUnsaved = create<{ by: Record<string, UnsavedKind> }>(() => ({ by: {} }));

/** Marks (or with null clears) the unsaved work under `key`. */
export function setUnsaved(key: string, kind: UnsavedKind | null) {
  const by = useUnsaved.getState().by;
  if ((by[key] ?? null) === kind) return;
  const next = { ...by };
  if (kind) next[key] = kind; else delete next[key];
  useUnsaved.setState({ by: next });
}

/** This component's unsaved work while `dirty`; it goes when the component does. */
export function useMarkUnsaved(kind: UnsavedKind, dirty: boolean) {
  const key = useId();
  useEffect(() => {
    setUnsaved(key, dirty ? kind : null);
    return () => setUnsaved(key, null);
  }, [key, kind, dirty]);
}

/** Pack Studio first, then the editors (the save editor's own store included); null: nothing unsaved. */
export function unsavedKind(by: Record<string, UnsavedKind>, saveEditorDirty: boolean): UnsavedKind | null {
  const kinds = Object.values(by);
  if (kinds.includes("studio")) return "studio";
  if (kinds.includes("editor") || saveEditorDirty) return "editor";
  return null;
}

/** The unsaved work there is now, as [`unsavedKind`] says it. */
export function useUnsavedKind(): UnsavedKind | null {
  const by = useUnsaved((s) => s.by);
  const edits = usePendingEdits((s) => s.dirty);
  return unsavedKind(by, edits);
}
