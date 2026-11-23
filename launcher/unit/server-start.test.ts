// The start's stages on Overview (src/pages/servers/startProgress.ts): which stages a start of a server goes through,
// which one it is in (the backend's stage names, or its lines), and the clock and "about N s left" of the start itself;
// the mock's restart publishes the same stages as kiln_manage::server::run.
import { QueryClient } from "@tanstack/react-query";
import { beforeEach, expect, test } from "bun:test";
import { onKilnEvent, serverRestart, serverStatus, taskGet, type Task } from "../src/api/commands";
import { runFlow, useServerFlows, type Flow, type FlowStep } from "../src/pages/servers/serverActions";
import { startTrack, startView } from "../src/pages/servers/startProgress";
import { useTasks } from "../src/ui/TaskProgress";

test("the mock restart publishes stopping and the five start stages", async () => {
  const stages: string[] = [];
  let finish!: (task: Task) => void;
  const done = new Promise<Task>((resolve) => { finish = resolve; });
  const unlisten = await onKilnEvent("kiln://task", (task) => {
    if (task.kind !== "server_restart") return;
    if (task.state !== "running") finish(task);
    else if (task.stage && stages[stages.length - 1] !== task.stage) stages.push(task.stage);
  });

  try {
    const id = await serverRestart("lab");
    const finished = await done;
    expect(stages).toEqual(["stopping", "preparing", "wine", "process", "world", "running"]);
    expect(finished).toMatchObject({ id, stage: "running", state: "done", result: { state: "running", lastStartS: expect.any(Number) } });
    expect(await taskGet(id)).toMatchObject({ stage: "running", state: "done" });
    expect((await serverStatus("lab")).lastStartS).toBeGreaterThanOrEqual(1);
  } finally {
    unlisten();
  }
}, 10_000);

const T0 = 1_000_000;
const step = (key: string, p: Partial<FlowStep> = {}): FlowStep => ({ key, label: key, state: "running", progress: null, lines: [], startedAt: T0, ...p });
const flow = (kind: Flow["kind"], s: FlowStep, steps: FlowStep[] = [s]): Flow => ({ seq: 1, kind, serverId: "lab", title: "x", steps, state: "running", startedAt: T0 });
const lines = (...texts: string[]) => texts.map((text, i) => ({ at: 1000 * (i + 1), text }));
const FIVE = ["preparing", "wine", "process", "world", "running"];

beforeEach(() => {
  useTasks.setState({ tasks: {}, order: [] });
  useServerFlows.setState({ flows: {}, specs: {} });
});

test("the track: Wine only where a Wine runs the server, a remote start prepares and runs, a restart stops first", () => {
  expect(startTrack({ runner: "kilnctl" })).toEqual(FIVE);
  expect(startTrack({ runner: "steam" })).toEqual(FIVE);
  expect(startTrack({ runner: "steam", onWindows: true })).toEqual(["preparing", "process", "world", "running"]);
  expect(startTrack({ runner: "windows" })).toEqual(["preparing", "process", "world", "running"]);
  expect(startTrack({ runner: "remote", remote: true })).toEqual(["preparing", "running"]);
  expect(startTrack({ runner: "kilnctl", restart: true })).toEqual(["stopping", ...FIVE]);
});

test("named stages: a restart's stop is a stage of its own, and the start's clock begins at its first preparing", () => {
  const track = startTrack({ runner: "kilnctl", restart: true });
  // still stopping: no start stage is current, no estimate; the clock is the stop's
  const stopping = step("restart", { stages: [{ id: "stopping", at: 0 }] });
  expect(startView(flow("restart", stopping), { stage: "stopping" }, T0 + 30_000, { track, lastStartS: 41 }))
    .toEqual({ stage: "stopping", since: T0, left: null, over: false });
  // the stop took 30 s: 10 s into the start itself, about 31 s are left of the last start's 41 (not 1 s)
  const s = step("restart", { stages: [{ id: "stopping", at: 0 }, { id: "preparing", at: 30_000 }, { id: "wine", at: 31_000 }, { id: "process", at: 32_000 }, { id: "world", at: 35_000 }] });
  expect(startView(flow("restart", s), { stage: "world" }, T0 + 40_000, { track, lastStartS: 41 })).toEqual({ stage: "world", since: T0 + 30_000, left: 31, over: false });
  // the stage of the task wins over the recorded ones (a stage event not seen yet)
  expect(startView(flow("restart", s), { stage: "running" }, T0 + 40_000, { track, lastStartS: 41 }).stage).toBe("running");
  // half a second before the last start's time: about 1 s, never "about 0 s"; past it: longer than last time
  expect(startView(flow("restart", s), { stage: "world" }, T0 + 70_500, { track, lastStartS: 41 }).left).toBe(1);
  expect(startView(flow("restart", s), { stage: "world" }, T0 + 71_000, { track, lastStartS: 41 })).toEqual({ stage: "world", since: T0 + 30_000, left: null, over: true });
  expect(startView(flow("restart", s), { stage: "world" }, T0 + 200_000, { track, lastStartS: 41 }).over).toBe(true);
  // no last start: no estimate either way
  expect(startView(flow("restart", s), { stage: "world" }, T0 + 40_000, { track })).toMatchObject({ left: null, over: false });
  // an update's start step (after stop and install) counts from its own preparing, not from the flow's start
  const start = step("start", { startedAt: T0 + 50_000, stages: [{ id: "preparing", at: 50_000 }, { id: "process", at: 51_000 }] });
  const upd = flow("deploy", start, [step("stop", { state: "done" }), step("deploy", { state: "done" }), start]);
  expect(startView(upd, { stage: "process" }, T0 + 60_000, { track: startTrack({ runner: "kilnctl" }), lastStartS: 20 })).toEqual({ stage: "process", since: T0 + 50_000, left: 10, over: false });
});

test("without stage names the lines say it, and a stage a track has not shows as the one before it", () => {
  const track = startTrack({ runner: "kilnctl" });
  const at = (l: FlowStep["lines"], kind: Flow["kind"] = "start", tr = track) =>
    startView(flow(kind, step(kind === "restart" ? "restart" : "start", { lines: l })), undefined, T0 + 10_000, { track: tr, lastStartS: 20 }).stage;
  expect(at([])).toBe("preparing");
  expect(at(lines("creating the Wine prefix"))).toBe("wine");
  expect(at(lines("creating the Wine prefix", "started GE-Proton10-3 ./enshrouded_server.exe (wine pid 5121), output in /x"))).toBe("process");
  expect(at(lines("started GE-Proton10-3 ./enshrouded_server.exe (wine pid 5121)", "[server] Loading world 'Lab'"))).toBe("world");
  // a restart: stopping until its stop's last line, then the start's lines; the clock from the start's first line
  const rtrack = startTrack({ runner: "kilnctl", restart: true });
  expect(at(lines("SIGINT -> pid 5120; waiting up to 90 s for the save", "waiting for wineserver"), "restart", rtrack)).toBe("stopping");
  const r = lines("SIGINT -> pid 5120; waiting up to 90 s for the save", "lab stopped, world saved", "started GE-Proton10-3 ./enshrouded_server.exe (wine pid 5122)", "[server] Loading world 'Lab'");
  expect(startView(flow("restart", step("restart", { lines: r })), undefined, T0 + 10_000, { track: rtrack, lastStartS: 20 })).toEqual({ stage: "world", since: T0 + 3000, left: 13, over: false });
  expect(at(lines("lab is not running"), "restart", rtrack)).toBe("preparing");
  // a Windows server's track has no Wine: an older backend's "wine" shows as preparing; a remote one's lines as preparing
  const win = startTrack({ runner: "windows" });
  expect(startView(flow("start", step("start", { stages: [{ id: "preparing", at: 0 }, { id: "wine", at: 10 }] })), { stage: "wine" }, T0, { track: win }).stage).toBe("preparing");
  expect(at(lines("started enshrouded_server.exe (pid 4242), output in C:\\x"), "start", win)).toBe("process");
  expect(at(lines("ssh vps-01 kilnctl start frostwood"), "start", startTrack({ remote: true }))).toBe("preparing");
});

test("started outside the launcher: only the world is known, and the clock is the process's uptime", () => {
  expect(startView(undefined, undefined, T0, { track: startTrack({ runner: "kilnctl" }), lastStartS: 41, uptimeS: 11 })).toEqual({ stage: "world", since: T0 - 11_000, left: 30, over: false });
  expect(startView(undefined, undefined, T0, { track: startTrack({ remote: true }) }).stage).toBe("preparing");
});

test("a flow keeps each stage its step's task names, with when it began", async () => {
  const qc = new QueryClient();
  const task: Task = { id: "named-start", kind: "server_start", title: "start", state: "running", progress: 0, message: null, started: "x", finished: null };
  const p = runFlow(qc, "lab", { kind: "start", title: "Starting", done: "Running", steps: [{ key: "start", label: "start", run: async () => {
    useTasks.getState().upsert({ ...task, stage: "preparing", message: "checking" });
    setTimeout(() => {
      useTasks.getState().upsert({ ...task, stage: "process", message: "started wine" });
      useTasks.getState().upsert({ ...task, stage: "process", message: "still the process" });
      useTasks.getState().upsert({ ...task, stage: "world", message: "[server] Loading world" });
      useTasks.getState().upsert({ ...task, stage: "running", state: "done", finished: "x", result: { state: "running" } });
    }, 5);
    return task.id;
  } }] });
  expect(await p).toBe(true);
  const st = useServerFlows.getState().flows.lab!.steps[0];
  expect(st.stages?.map((s) => s.id)).toEqual(["preparing", "process", "world", "running"]);
  expect(st.stages!.every((s, i, a) => s.at >= 0 && (i === 0 || s.at >= a[i - 1].at))).toBe(true);
});
