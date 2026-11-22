// Servers revamp, part 1 (2026-11-22): every server action is a flow of task steps whose progress lines the server's
// head shows (src/pages/servers/serverActions.ts). A running server's Kiln update is stop, install, start again; the
// install's note and notice come as soon as it ends; a failed step ends the flow with its reason and skips the rest, and
// Try again goes on from that step; a second flow of the same server is refused unless it takes over (Stop during a
// start). The head's word comes from the running step, never from a write into the status query.
import { QueryClient } from "@tanstack/react-query";
import { beforeEach, expect, test } from "bun:test";
import type { ServerStatus, Task } from "../src/api/commands";
import { elapsed, flowPhase, retryFlow, runFlow, useServerFlows, type FlowPhase, type StepSpec } from "../src/pages/servers/serverActions";
import { sk } from "../src/pages/servers/serverApi";
import { useTasks } from "../src/ui/TaskProgress";

const base = (id: string, kind: string): Task => ({ id, kind, title: kind, state: "running", progress: 0, message: null, started: "2026-11-22T06:00:00Z", finished: null });
const tick = () => new Promise((r) => setTimeout(r, 0));
/** task ids unique across tests: a late event of an earlier test's task must not end this test's */
let uid = 0;

interface Fake extends StepSpec { ran: () => number; release: () => void }

/** A step whose task reports `lines` then ends (done, or failed with `fail`), driven through useTasks. `hold`: it ends
 *  only after release(). */
function fakeStep(key: string, lines: string[], opts: { fail?: string; phase?: FlowPhase; result?: unknown; isResult?: boolean; hold?: boolean } = {}): Fake {
  let runs = 0;
  let release = () => {};
  return {
    key, label: key, phase: opts.phase, result: opts.isResult,
    ran: () => runs,
    release: () => release(),
    run: async () => {
      runs++;
      const id = `${key}-${runs}-${++uid}`;
      const t = base(id, key);
      useTasks.getState().upsert(t);
      const gate = opts.hold ? new Promise<void>((r) => { release = r; }) : Promise.resolve();
      setTimeout(async () => {
        for (const [i, m] of lines.entries()) { useTasks.getState().upsert({ ...t, message: m, progress: (i + 1) / (lines.length + 1) }); await tick(); }
        // a repeated line is kept once
        if (lines.length) useTasks.getState().upsert({ ...t, message: lines[lines.length - 1], progress: 0.99 });
        await gate;
        useTasks.getState().upsert(opts.fail
          ? { ...t, state: "failed", message: opts.fail, error: { code: "io", message: opts.fail }, finished: "x" }
          : { ...t, state: "done", progress: 1, message: lines[lines.length - 1] ?? null, result: opts.result ?? null, finished: "x" });
      }, 0);
      return id;
    },
  };
}

const status = (state: ServerStatus["state"]): ServerStatus => ({ id: "lab", state, pid: state === "stopped" ? null : 1, uptimeS: null, players: null, kiln: null, installed: null, boot: null, endpoint: null } as ServerStatus);

beforeEach(() => {
  useTasks.setState({ tasks: {}, order: [] });
  useServerFlows.setState({ flows: {}, specs: {} });
});

test("an update of a running server runs stop, install, start again, keeping each step's lines", async () => {
  const qc = new QueryClient();
  qc.setQueryData(sk.status("lab"), status("running"));
  const stop = fakeStep("stop", ["SIGINT -> pid 1", "waiting for wineserver"], { phase: "stopping" });
  const install = fakeStep("deploy", ["Verifying the bundle", "Placing files"], { result: { receipt: 1 }, isResult: true, phase: "updating" });
  const start = fakeStep("start", ["started wine", "[ecs] LoadEcsScene to Load"], { phase: "starting", result: { state: "running" } });
  let after: unknown = null;
  const p = runFlow<{ receipt: number }>(qc, "lab", { kind: "deploy", title: "Updating Lab", steps: [stop, install, start], done: "Kiln installed", detail: (r) => `receipt ${r.receipt}`, after: (r) => { after = r; } });
  // the head's word is the running step's; the status query keeps the backend's word
  expect(flowPhase(useServerFlows.getState().flows.lab)).toBe("stopping");
  expect(qc.getQueryData<ServerStatus>(sk.status("lab"))?.state).toBe("running");
  expect(useServerFlows.getState().flows.lab?.state).toBe("running");
  expect(await p).toBe(true);
  const f = useServerFlows.getState().flows.lab!;
  expect(f.state).toBe("done");
  expect(flowPhase(f)).toBeNull();
  expect(f.steps.map((s) => s.state)).toEqual(["done", "done", "done"]);
  expect(f.steps[0].lines.map((l) => l.text)).toEqual(["SIGINT -> pid 1", "waiting for wineserver"]);
  expect(f.steps[2].lines.map((l) => l.text)).toEqual(["started wine", "[ecs] LoadEcsScene to Load"]);
  expect(f.doneText).toBe("Kiln installed");
  // the install's result is the flow's (its note and the install notice), not the start's that follows it
  expect(f.detail).toBe("receipt 1");
  expect(after).toEqual({ receipt: 1 });
});

test("a start that fails after the install keeps the install's note, and Try again only starts", async () => {
  const qc = new QueryClient();
  const stop = fakeStep("stop", ["SIGINT -> pid 1"]);
  const install = fakeStep("deploy", ["Placing files"], { result: { receipt: 7 }, isResult: true });
  const start = fakeStep("start", ["started wine"], { fail: "the server exited before its world ran (exit code 1)" });
  const notices: unknown[] = [];
  expect(await runFlow<{ receipt: number }>(qc, "lab", { kind: "deploy", title: "Updating Lab", steps: [stop, install, start], done: "Kiln installed",
    detail: (r) => `receipt ${r.receipt}`, after: (r) => notices.push(r) })).toBe(false);
  const f = useServerFlows.getState().flows.lab!;
  expect(f.steps.map((s) => s.state)).toEqual(["done", "done", "failed"]);
  expect(f.detail).toBe("receipt 7");
  expect(notices).toEqual([{ receipt: 7 }]);
  await retryFlow(qc, "lab");
  expect([stop.ran(), install.ran(), start.ran()]).toEqual([1, 1, 2]);
  expect(useServerFlows.getState().flows.lab?.steps.map((s) => s.key)).toEqual(["start"]);
  expect(notices).toHaveLength(1);
});

test("a failed step ends the flow with its reason, skips the rest, and Try again goes on from it", async () => {
  const qc = new QueryClient();
  const stop = fakeStep("stop", ["SIGINT -> pid 1"]);
  const install = fakeStep("deploy", ["Verifying the bundle"], { fail: "No runtime bundle 0.2.0-dev.30.9: import it on the Runtime page." });
  const start = fakeStep("start", ["started wine"]);
  expect(await runFlow(qc, "lab", { kind: "deploy", title: "Updating Lab", steps: [stop, install, start], done: "Kiln installed" })).toBe(false);
  const f = useServerFlows.getState().flows.lab!;
  expect(f.state).toBe("failed");
  expect(f.error?.message).toContain("No runtime bundle");
  expect(f.steps.map((s) => s.state)).toEqual(["done", "failed", "skipped"]);
  expect(start.ran()).toBe(0);
  expect(await retryFlow(qc, "lab")).toBe(false);
  // the stop was done: not again
  expect([stop.ran(), install.ran(), start.ran()]).toEqual([1, 2, 0]);
});

test("a second flow of the same server is refused while one runs; another server's is not", async () => {
  const qc = new QueryClient();
  const a = runFlow(qc, "lab", { kind: "start", title: "Starting Lab", steps: [fakeStep("start", ["started wine"])], done: "Running" });
  expect(await runFlow(qc, "lab", { kind: "update", title: "Updating Lab", steps: [fakeStep("update", [])], done: "Up to date" })).toBe(false);
  const b = runFlow(qc, "other", { kind: "start", title: "Starting Other", steps: [fakeStep("start", [])], done: "Running" });
  expect(await a).toBe(true);
  expect(await b).toBe(true);
  expect(useServerFlows.getState().flows.lab?.kind).toBe("start");
  // a finished flow can be dismissed; a running one cannot
  useServerFlows.getState().dismiss("lab");
  expect(useServerFlows.getState().flows.lab).toBeUndefined();
});

test("Stop during a start takes over: the start ends as canceled and its late events change nothing", async () => {
  const qc = new QueryClient();
  const start = fakeStep("start", ["started wine"], { hold: true, phase: "starting" });
  const starting = runFlow(qc, "lab", { kind: "start", title: "Starting Lab", steps: [start], done: "Running" });
  await tick(); await tick();
  const now = useServerFlows.getState().flows.lab;
  expect({ state: now?.state, steps: now?.steps.map((s) => [s.state, s.phase]) }).toEqual({ state: "running", steps: [["running", "starting"]] });
  expect(flowPhase(now)).toBe("starting");
  const stopping = runFlow(qc, "lab", { kind: "stop", title: "Stopping Lab", steps: [fakeStep("stop", ["SIGINT -> pid 1"], { phase: "stopping" })], done: "Stopped" }, { supersede: true });
  expect(await stopping).toBe(true);
  // the old start's task ends late: the stop's flow stays
  start.release();
  expect(await starting).toBe(true);
  const f = useServerFlows.getState().flows.lab!;
  expect(f.kind).toBe("stop");
  expect(f.state).toBe("done");
  expect(f.doneText).toBe("Stopped");
});

test("a step whose command is refused at once fails the flow with the command's error", async () => {
  const qc = new QueryClient();
  const refused: StepSpec = { key: "start", label: "start", run: () => Promise.reject({ code: "conflict", message: "lab is running already (pid 5120)" }) };
  expect(await runFlow(qc, "lab", { kind: "start", title: "Starting Lab", steps: [refused], done: "Running" })).toBe(false);
  expect(useServerFlows.getState().flows.lab?.error?.message).toBe("lab is running already (pid 5120)");
});

test("without a marked step the last step's result is the flow's", async () => {
  const qc = new QueryClient();
  let after: unknown = null;
  await runFlow(qc, "lab", { kind: "update", title: "Updating", steps: [fakeStep("stop", [], { result: "a" }), fakeStep("update", [], { result: "b" })], done: "Up to date", after: (r) => { after = r; } });
  expect(after).toBe("b");
});

test("elapsed time reads as seconds, then minutes", () => {
  expect(elapsed(0)).toBe("0 s");
  expect(elapsed(59_900)).toBe("59 s");
  expect(elapsed(72_000)).toBe("1:12");
  expect(elapsed(3_600_000)).toBe("60:00");
});
