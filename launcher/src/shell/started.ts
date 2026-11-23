// The page tells the backend once that its first screen is up: `app_started`, after React's first commit and two
// frames (the second frame callback runs once the first frame was painted). On Linux the backend's render watchdog
// (src-tauri/src/render.rs) starts the launcher again without the GPU when this call never comes. Desktop and Deck UI
// alike: main.tsx mounts <Started /> next to the app. Only startup visibility/frame checks; a failure is ignored.
//
// A hidden page (the window minimized or on another workspace at the start) gets no frames until it is shown, so it
// says it started at once, or when it is hidden before its frames came: there is no black window anyone looks at, and
// waiting would make the watchdog switch a working GPU off. This readiness does NOT confirm a painted frame:
// app_render_frame reports visibility separately and acknowledges two visible frames, even after a hidden startup.
import { useEffect } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import * as api from "../api/commands";
import { t } from "../i18n";
import { toast } from "../ui";

/** A function that sends the ready call the first time it runs, after two frames (at once when `hidden()`, or when
 *  `onHidden` reports the page hidden before them), and never again. */
export function startedReporter(
  send: () => Promise<unknown>,
  frame: (cb: () => void) => void,
  hidden: () => boolean = () => false,
  onHidden: (cb: () => void) => void = () => {},
  painted: () => Promise<unknown> = async () => {},
  shown: () => boolean | Promise<boolean> = () => true,
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
            painted().catch(() => {});
          });
        });
      });
    };
    frame(first);
  };
}

const pageHidden = () => typeof document !== "undefined" && document.visibilityState === "hidden";

export const tellStarted = startedReporter(
  () => api.appStarted(),
  (cb) => { requestAnimationFrame(() => cb()); },
  pageHidden,
  (cb) => {
    const on = () => {
      const hidden = pageHidden();
      api.appRenderFrame(false, !hidden).catch(() => {});
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
    await api.appRenderFrame(false, visible).catch(() => {});
    return visible;
  },
);

/** Mounted once at the root: its effect runs after React's first commit. */
export function Started() {
  useEffect(() => tellStarted(), []);
  return null;
}
