// Whether the save editor holds changes that are not written yet, and for which character: a tiny store with no editor
// data behind it, so the shells (the Deck's Quit dialog) can ask without loading the editor's chunk. session.js keeps it.
import { create } from "zustand";

export const usePendingEdits = create<{ dirty: boolean; busy: boolean; name: string | null }>(() => ({ dirty: false, busy: false, name: null }));

// The session survives page/tab navigation, so the reload warning must survive it too. Browsers show their own
// localized warning; native window-close interception belongs to the launcher shell, not this store.
if (typeof window !== "undefined") {
  window.addEventListener("beforeunload", (event) => {
    const { dirty, busy } = usePendingEdits.getState();
    if (!dirty && !busy) return;
    event.preventDefault();
    event.returnValue = "";
  });
}
