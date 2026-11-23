// The page tells the backend once that its first screen is up: `app_started`, after React's first commit and two
// frames (the second frame callback runs once the first frame was painted). On Linux the backend's render watchdog
// (src-tauri/src/render.rs) starts the launcher again without the GPU when this call never comes. Desktop and Deck UI
// alike: main.tsx mounts <Started /> next to the app. Only startup visibility/frame checks; a failure is ignored.
//
// A hidden page (the window minimized or on another workspace at the start) gets no frames until it is shown, so it
// says it started at once, or when it is hidden before its frames came: there is no black window anyone looks at, and
// waiting would make the watchdog switch a working GPU off. This readiness does NOT confirm a painted frame:
// app_render_frame reports visibility separately and acknowledges two visible frames, even after a hidden startup.
//
// The backend keeps its renderer state in the data folder (render.rs), so app_render_frame waits while the folder is
// held: until the data folder gate (shell/DataFolderGate.tsx) lets the launcher mount, and while it shows a pointer to
// a folder that cannot be used, another launcher's move or the removal of an unfinished move's copy. Once the hold ends,
// the latest visibility goes out and two fresh visible frames confirm painting. app_started goes out in every case.
import { useEffect } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import * as api from "../api/commands";
import { t } from "../i18n";
import { toast } from "../ui";

/** Whether the data folder is held, as the data folder gate knows it (held until it has answered). `whenFree` runs its
 *  callback at once when the folder is free, else when `set(false)` ends the hold. */
export interface FolderHold {
  held(): boolean;
  set(held: boolean): void;
  whenFree(cb: () => void): void;
}

export function folderHold(held = true): FolderHold {
  const waiting: (() => void)[] = [];
  return {
    held: () => held,
    set(next) {
      held = next;
      while (!held && waiting.length) waiting.shift()!();
    },
    whenFree(cb) {
      if (held) waiting.push(cb);
      else cb();
    },
  };
}

/** `send` at once while `hold` is free; while it is held, one `send` with the latest arguments once the hold ends. */
export function afterHold<A extends unknown[]>(hold: FolderHold, send: (...a: A) => void): (...a: A) => void {
  let queued: A | null = null;
  return (...a) => {
    if (!hold.held()) { send(...a); return; }
    const first = queued === null;
    queued = a;
    if (first) hold.whenFree(() => { const q = queued!; queued = null; send(...q); });
  };
}

/** A function that sends the ready call the first time it runs, after two frames (at once when `hidden()`, or when
 *  `onHidden` reports the page hidden before them), and never again. `painted` waits while `hold` is held: two fresh
 *  visible frames after the hold ends confirm painting. */
export function startedReporter(
  send: () => Promise<unknown>,
  frame: (cb: () => void) => void,
  hidden: () => boolean = () => false,
  onHidden: (cb: () => void) => void = () => {},
  painted: () => Promise<unknown> = async () => {},
  shown: () => boolean | Promise<boolean> = () => true,
  hold: FolderHold = folderHold(false),
): () => void {
  let told = false;
  let sent = false;
  let visibilityEpoch = 0;
  const go = () => {
    if (sent) return;
    sent = true;
    send().catch(() => {});
  };
  return () => {
    if (told) return;
    told = true;
    if (hidden()) go();
    onHidden(() => { visibilityEpoch += 1; go(); });
    const whileShown = (next: () => void) => {
      const confirm = (visible: boolean) => {
        if (!visible || hidden()) { go(); frame(first); return; }
        next();
      };
      const visible = shown();
      if (typeof visible === "boolean") confirm(visible);
      else visible.then(confirm).catch(() => { go(); frame(first); });
    };
    const first = () => {
      if (hidden()) { frame(first); return; }
      whileShown(() => {
        const epoch = visibilityEpoch;
        frame(() => {
          if (epoch !== visibilityEpoch || hidden()) { frame(first); return; }
          go();
          whileShown(() => {
            if (epoch !== visibilityEpoch) { frame(first); return; }
            // the data folder is held: no frame report until it is free, then two fresh visible frames
            if (hold.held()) { hold.whenFree(() => frame(first)); return; }
            painted().catch(() => {});
          });
        });
      });
    };
    frame(first);
  };
}

const pageHidden = () => typeof document !== "undefined" && document.visibilityState === "hidden";

/** The data folder's hold for frame reports: DataFolderGate sets it (held until data_folder_get has answered). */
export const dataFolderHold = folderHold();

const reportVisibility = afterHold(dataFolderHold, (visible: boolean) => { api.appRenderFrame(false, visible).catch(() => {}); });

export const tellStarted = startedReporter(
  () => api.appStarted(),
  (cb) => { requestAnimationFrame(() => cb()); },
  pageHidden,
  (cb) => {
    const on = () => {
      const hidden = pageHidden();
      reportVisibility(!hidden);
      if (hidden) cb();
    };
    document.addEventListener("visibilitychange", on);
    on();
  },
  async () => {
    const reason = await api.appRenderFrame(true, true);
    if (reason) toast.info(t("settings.render.recoveryTitle"), reason);
  },
  async () => {
    if (!api.isTauri()) return true;
    const window = getCurrentWindow();
    const visible = await window.isVisible() && !await window.isMinimized();
    // while held, the frames after the hold's end report it
    if (!dataFolderHold.held()) await api.appRenderFrame(false, visible).catch(() => {});
    return visible;
  },
  dataFolderHold,
);

/** Mounted once at the root: its effect runs after React's first commit. */
export function Started() {
  useEffect(() => tellStarted(), []);
  return null;
}
