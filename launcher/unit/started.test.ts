// The page tells the backend once that its first screen is up (src/shell/started.ts; the backend's render watchdog
// falls back to software mode on Linux when that call never comes).
import { expect, test } from "bun:test";
import { afterHold, folderHold, startedReporter } from "../src/shell/started";

function harness() {
  const sent: number[] = [];
  const frames: (() => void)[] = [];
  const tell = startedReporter(async () => { sent.push(sent.length + 1); }, (cb) => { frames.push(cb); });
  const frame = () => frames.shift()?.();
  return { sent, frames, tell, frame };
}

test("the ready call goes out once, however often the first commit's effect runs", async () => {
  const h = harness();
  // React's StrictMode runs a mount effect twice in development; a remount runs it again
  h.tell(); h.tell(); h.tell();
  expect(h.frames.length).toBe(1);
  h.frame(); h.frame();
  await Promise.resolve();
  expect(h.sent).toEqual([1]);
  h.tell();
  expect(h.frames.length).toBe(0);
  await Promise.resolve();
  expect(h.sent).toEqual([1]);
});

test("the ready call waits for two frames after the commit (the first screen was painted)", async () => {
  const h = harness();
  h.tell();
  await Promise.resolve();
  expect(h.sent).toEqual([]);
  h.frame();
  await Promise.resolve();
  expect(h.sent).toEqual([]);
  h.frame();
  await Promise.resolve();
  expect(h.sent).toEqual([1]);
});

test("a page hidden at the start says it started at once, without waiting for frames", async () => {
  const sent: number[] = [];
  const frames: (() => void)[] = [];
  const tell = startedReporter(async () => { sent.push(1); }, (cb) => { frames.push(cb); }, () => true);
  tell(); tell();
  await Promise.resolve();
  expect(sent).toEqual([1]);
  expect(frames.length).toBe(1);
});

test("a page hidden before its frames came says it started then, and only once", async () => {
  const sent: number[] = [];
  const frames: (() => void)[] = [];
  let hide: (() => void) | undefined;
  const tell = startedReporter(async () => { sent.push(1); }, (cb) => { frames.push(cb); }, () => false, (cb) => { hide = cb; });
  tell();
  await Promise.resolve();
  expect(sent).toEqual([]);
  hide?.();
  await Promise.resolve();
  expect(sent).toEqual([1]);
  // the frames come when it is shown again: no second call
  frames.shift()?.(); frames.shift()?.();
  hide?.();
  await Promise.resolve();
  expect(sent).toEqual([1]);
});

test("a failed ready call is not retried and does not throw", async () => {
  let calls = 0;
  const tell = startedReporter(() => { calls += 1; return Promise.reject(new Error("no backend")); }, (cb) => cb());
  expect(() => { tell(); tell(); }).not.toThrow();
  await new Promise((r) => setTimeout(r, 0));
  expect(calls).toBe(1);
});


test("a hidden ready call does not acknowledge a painted frame; two visible frames do", async () => {
  const frames: (() => void)[] = [];
  const ready: string[] = [];
  let hidden = true;
  const tell = startedReporter(async () => { ready.push("ready"); }, (cb) => frames.push(cb), () => hidden, () => {},
    async () => { ready.push("painted"); });
  tell();
  expect(ready).toEqual(["ready"]);
  hidden = false;
  frames.shift()?.();
  expect(ready).toEqual(["ready"]);
  frames.shift()?.();
  await Promise.resolve();
  expect(ready).toEqual(["ready", "painted"]);
});


test("frames while hidden are not painting success and confirmation waits for two visible frames", async () => {
  const frames: (() => void)[] = [];
  const events: string[] = [];
  let hidden = true;
  const tell = startedReporter(async () => { events.push("ready"); }, (cb) => frames.push(cb), () => hidden, () => {}, async () => { events.push("painted"); });
  tell();
  frames.shift()?.();
  expect(events).toEqual(["ready"]);
  hidden = false;
  frames.shift()?.();
  expect(events).toEqual(["ready"]);
  frames.shift()?.();
  expect(events).toEqual(["ready", "painted"]);
  tell();
  expect(frames).toEqual([]);
});

test("hiding between callbacks invalidates the first visible frame", async () => {
  const frames: (() => void)[] = [];
  const events: string[] = [];
  let hide: (() => void) | undefined;
  const tell = startedReporter(async () => { events.push("ready"); }, (cb) => frames.push(cb), () => false,
    (cb) => { hide = cb; }, async () => { events.push("painted"); });
  tell();
  frames.shift()?.(); // first visible frame
  hide?.(); // hidden, then shown again before the second callback
  frames.shift()?.();
  expect(events).toEqual(["ready"]);
  frames.shift()?.();
  frames.shift()?.();
  expect(events).toEqual(["ready", "painted"]);
});

test("a document that is visible while its native window is hidden cannot confirm painting", () => {
  const frames: (() => void)[] = [];
  const events: string[] = [];
  let shown = false;
  const tell = startedReporter(async () => { events.push("ready"); }, (cb) => frames.push(cb), () => false,
    () => {}, async () => { events.push("painted"); }, () => shown);
  tell();
  frames.shift()?.();
  shown = true; // becoming visible only for the second callback is not two visible frames
  frames.shift()?.();
  expect(events).toEqual(["ready"]);
  frames.shift()?.();
  expect(events).toEqual(["ready", "painted"]);
});

// The data folder held at the start (a pointer to a folder that cannot be used, another launcher's move, an unfinished
// move's removal: shell/DataFolderGate.tsx): the backend keeps its renderer state there, so no frame report goes out.
test("a held data folder: the ready call goes out, frame reports wait for the hold's end and two fresh visible frames", () => {
  const frames: (() => void)[] = [];
  const events: string[] = [];
  const hold = folderHold();
  const tell = startedReporter(async () => { events.push("ready"); }, (cb) => frames.push(cb), () => false, () => {},
    async () => { events.push("painted"); }, () => true, hold);
  tell();
  frames.shift()?.();
  frames.shift()?.();
  // the watchdog still hears that the page is up
  expect(events).toEqual(["ready"]);
  // nothing waits on a frame while the folder is held
  expect(frames).toEqual([]);
  hold.set(false);
  expect(events).toEqual(["ready"]);
  frames.shift()?.();
  expect(events).toEqual(["ready"]);
  frames.shift()?.();
  expect(events).toEqual(["ready", "painted"]);
  expect(frames).toEqual([]);
});

test("a hold that never ends sends no frame report", () => {
  const frames: (() => void)[] = [];
  const events: string[] = [];
  const hold = folderHold();
  const tell = startedReporter(async () => { events.push("ready"); }, (cb) => frames.push(cb), () => false, () => {},
    async () => { events.push("painted"); }, () => true, hold);
  tell();
  for (let i = 0; i < 6; i++) frames.shift()?.();
  hold.set(true);
  expect(events).toEqual(["ready"]);
  expect(frames).toEqual([]);
});

test("the hold: at once when free, queued while held, held again after a late answer", () => {
  const hold = folderHold();
  const ran: string[] = [];
  expect(hold.held()).toBe(true);
  hold.whenFree(() => ran.push("a"));
  hold.whenFree(() => ran.push("b"));
  hold.set(true);
  expect(ran).toEqual([]);
  hold.set(false);
  expect(ran).toEqual(["a", "b"]);
  hold.whenFree(() => ran.push("c"));
  expect(ran).toEqual(["a", "b", "c"]);
  // the gate's late answer with a problem takes the window again
  hold.set(true);
  hold.whenFree(() => ran.push("d"));
  expect(ran).toEqual(["a", "b", "c"]);
  hold.set(false);
  expect(ran).toEqual(["a", "b", "c", "d"]);
  expect(folderHold(false).held()).toBe(false);
});

test("visibility reports while held: one, with the latest visibility, once the hold ends", () => {
  const hold = folderHold();
  const sent: boolean[] = [];
  const report = afterHold(hold, (visible: boolean) => { sent.push(visible); });
  report(true);
  report(false);
  report(true);
  report(false);
  expect(sent).toEqual([]);
  hold.set(false);
  expect(sent).toEqual([false]);
  report(true);
  expect(sent).toEqual([false, true]);
  hold.set(true);
  report(true);
  hold.set(false);
  expect(sent).toEqual([false, true, true]);
});
