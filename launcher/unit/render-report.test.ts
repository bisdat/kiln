// shell/renderReport.ts: the frame sample's numbers and its life (inbox 94: a slow path shows as numbers in launcher.log).
import { describe, expect, test } from "bun:test";
import { FRAME_DELAY, FRAME_MS, frameStats, sampleFrames, type FrameDeps } from "../src/shell/renderReport";

describe("frameStats", () => {
  test("gaps between frames: count, median, 95th percentile, longest, the ones over 50 ms", () => {
    // 60 a second, then two long frames
    const t = [0];
    for (let i = 0; i < 18; i += 1) t.push(t[t.length - 1] + 16.7);
    t.push(t[t.length - 1] + 66, t[t.length - 1] + 66 + 120);
    const s = frameStats(t)!;
    expect(s.frames).toBe(20);
    expect(s.p50).toBe(16.7);
    expect(s.max).toBe(120);
    expect(s.p95).toBe(120);
    expect(s.slow).toBe(2);
    expect(s.ms).toBe(Math.round((18 * 16.7 + 186) * 10) / 10);
  });
  test("too few frames say nothing", () => {
    expect(frameStats([])).toBeNull();
    expect(frameStats([0, 16])).toBeNull();
  });
});

/** A clock of frames 16 ms apart and one timer, driven by hand. */
function fake(ok: () => boolean) {
  let now = 0, timer: (() => void) | null = null;
  const frames: ((t: number) => void)[] = [];
  const sent: unknown[] = [];
  const d: FrameDeps = {
    ok,
    frame: (cb) => { frames.push(cb); return frames.length; },
    cancelFrame: () => { frames.length = 0; },
    wait: (cb, ms) => { expect(ms).toBe(FRAME_DELAY); timer = cb; return 1; },
    cancelWait: () => { timer = null; },
    send: (s) => sent.push(s),
  };
  return {
    d, sent,
    fire: () => { const t = timer; timer = null; t?.(); },
    run: (n: number) => { for (let i = 0; i < n && frames.length; i += 1) { const cb = frames.shift()!; cb(now); now += 16; } },
  };
}

describe("sampleFrames", () => {
  test("waits, samples FRAME_MS of frames, sends once", () => {
    const f = fake(() => true);
    sampleFrames(f.d);
    f.run(10);
    expect(f.sent).toHaveLength(0); // nothing before the delay
    f.fire();
    f.run(1000);
    expect(f.sent).toHaveLength(1);
    const s = f.sent[0] as { frames: number; ms: number; p50: number };
    expect(s.ms).toBeGreaterThanOrEqual(FRAME_MS);
    expect(s.p50).toBe(16);
  });
  test("leaving Play (or a hidden window) drops the sample", () => {
    let on = true;
    const f = fake(() => on);
    sampleFrames(f.d);
    f.fire();
    f.run(20);
    on = false;
    f.run(1000);
    expect(f.sent).toHaveLength(0);
  });
  test("cancelled before the delay: no frames are asked for", () => {
    const f = fake(() => true);
    const cancel = sampleFrames(f.d);
    cancel();
    f.fire();
    f.run(1000);
    expect(f.sent).toHaveLength(0);
  });
});
