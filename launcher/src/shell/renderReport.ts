// What the page can tell about how WebKit draws it, for launcher.log (inbox 94: an RTX 5080 on KDE Wayland where WebKit
// composited nothing although the launcher drew with the GPU, and nothing in the log said so). Two lines a start at most
// (`app_render_report`, src-tauri render.rs): the facts once the first screen is up (does WebKit composite, WebGL, the
// window, devicePixelRatio, reduced motion, the fire), and one frame sample on Play (requestAnimationFrame over FRAME_MS
// while the window is visible), so a slow path shows as numbers. Cheap: a throwaway WebGL context, released at once,
// and one IPC call each; a failure is ignored.
import * as api from "../api/commands";
import type { FrameSample, PageRender } from "../api/commands";
import { flatDrawing } from "./Stage";

/** How long the frame sample runs (ms), and how long Play must have been up before it starts (the mouth's spring, the
 *  page's first paints). */
export const FRAME_MS = 2000;
export const FRAME_DELAY = 1500;

/** The gaps between frame times (ms, rising): count, median, 95th percentile, the longest, the ones over 50 ms. */
export function frameStats(times: number[]): FrameSample | null {
  if (times.length < 3) return null;
  const gaps: number[] = [];
  for (let i = 1; i < times.length; i += 1) gaps.push(times[i] - times[i - 1]);
  const sorted = [...gaps].sort((a, b) => a - b);
  const at = (p: number) => sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))];
  const r = (n: number) => Math.round(n * 10) / 10;
  return {
    ms: r(times[times.length - 1] - times[0]),
    frames: gaps.length,
    p50: r(at(0.5)),
    p95: r(at(0.95)),
    max: r(sorted[sorted.length - 1]),
    slow: gaps.filter((g) => g > 50).length,
  };
}

/** WebGL's version and renderer ("none" without it), from a throwaway context that is released at once. */
export function webglName(doc: Document = document): string {
  try {
    const gl = doc.createElement("canvas").getContext("webgl");
    if (!gl) return "none";
    const dbg = gl.getExtension("WEBGL_debug_renderer_info");
    const renderer = dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
    const name = `${gl.getParameter(gl.VERSION)} / ${renderer}`;
    gl.getExtension("WEBGL_lose_context")?.loseContext();
    return name;
  } catch {
    return "none";
  }
}

/** The facts line: what this page sees of WebKit's drawing. */
export function pageFacts(fire: boolean): PageRender {
  return {
    composited: !flatDrawing(),
    webgl: webglName(),
    dpr: window.devicePixelRatio,
    width: window.innerWidth,
    height: window.innerHeight,
    reducedMotion: !!window.matchMedia?.("(prefers-reduced-motion: reduce)").matches,
    fire,
  };
}

let factsSent = false;
/** The facts, once a start. */
export function reportFacts(fire: boolean) {
  if (factsSent) return;
  factsSent = true;
  api.appRenderReport(pageFacts(fire)).catch(() => {});
}

export interface FrameDeps {
  /** Still on Play with the window visible. */
  ok: () => boolean;
  frame: (cb: (t: number) => void) => number;
  cancelFrame: (id: number) => void;
  wait: (cb: () => void, ms: number) => number;
  cancelWait: (id: number) => void;
  send: (s: FrameSample) => void;
}

/** One frame sample: FRAME_DELAY after it is asked for, FRAME_MS of frame times; dropped (nothing sent) when `ok` turns
 *  false meanwhile. Returns the cancel. */
export function sampleFrames(d: FrameDeps): () => void {
  let raf = 0, done = false;
  const times: number[] = [];
  const tick = (t: number) => {
    if (done) return;
    if (!d.ok()) { done = true; return; }
    times.push(t);
    if (t - times[0] < FRAME_MS) { raf = d.frame(tick); return; }
    done = true;
    const s = frameStats(times);
    if (s) d.send(s);
  };
  const timer = d.wait(() => { if (!done && d.ok()) raf = d.frame(tick); else done = true; }, FRAME_DELAY);
  return () => { done = true; d.cancelWait(timer); if (raf) d.cancelFrame(raf); };
}

let framesSent = false;
/** The frame sample on Play, once a start: call it each time Play shows (or the window comes back); the cancel goes in
 *  the effect's cleanup. A sample cut short is tried again the next time. */
export function reportFrames(onPlay: () => boolean): () => void {
  if (framesSent) return () => {};
  return sampleFrames({
    ok: () => onPlay() && document.visibilityState !== "hidden",
    frame: (cb) => requestAnimationFrame(cb),
    cancelFrame: (id) => cancelAnimationFrame(id),
    wait: (cb, ms) => window.setTimeout(cb, ms),
    cancelWait: (id) => clearTimeout(id),
    send: (frames) => {
      if (framesSent) return;
      framesSent = true;
      api.appRenderReport({ frames }).catch(() => {});
    },
  });
}
