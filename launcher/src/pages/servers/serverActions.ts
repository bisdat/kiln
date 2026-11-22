// Server actions with feedback. Every Start, Stop, Restart, game update and Kiln install runs as a flow: one or more
// task steps (for an update of a running server: stop, install, start again). The store keeps, per server, the flow's
// steps, each step's task and the distinct progress lines the task reported, so the server's head shows what happens
// now, how long it has taken and, at the end, the result or the reason it failed. The head takes its word (starting,
// stopping, updating) from the running step, never by writing into the status query, which keeps the backend's word.
// The status query polls every second while a flow runs or the server is starting or stopping (useServerStatus).
import type { QueryClient } from "@tanstack/react-query";
import { create } from "zustand";
import { taskCancel, type KilnError, type ServerId, type Task, type TaskId } from "../../api/commands";
import { toast, useTasks, waitTask } from "../../ui";
import { sk } from "./serverApi";

export type StepState = "pending" | "running" | "done" | "failed" | "canceled" | "skipped";
export type FlowState = "running" | "done" | "failed" | "canceled";
export type FlowKind = "start" | "stop" | "restart" | "update" | "deploy";
/** What the head says while a step runs. */
export type FlowPhase = "starting" | "stopping" | "updating";
/** How a running step can be ended early: "cancel" ends the task (an install, an update); "stopWaiting" ends a start's
 *  wait (the server keeps starting: kiln_manage::server::run::wait_ready). */
export type StepCancel = "cancel" | "stopWaiting";

export interface FlowLine {
  /** ms since the flow started */
  at: number;
  text: string;
}

export interface FlowStep {
  key: string;
  label: string;
  state: StepState;
  taskId?: TaskId;
  /** the task's progress (0..1) when it reports one */
  progress: number | null;
  /** the distinct progress lines the step's task reported, oldest first (at most MAX_LINES) */
  lines: FlowLine[];
  startedAt?: number;
  endedAt?: number;
  phase?: FlowPhase;
  cancel?: StepCancel;
}

export interface Flow {
  /** unique per flow (a new flow replaces a finished one) */
  seq: number;
  kind: FlowKind;
  serverId: ServerId;
  /** "Starting Lab server" */
  title: string;
  steps: FlowStep[];
  state: FlowState;
  startedAt: number;
  endedAt?: number;
  error?: KilnError;
  /** the result line ("Running.", "Kiln 0.2.0-dev.30.4 installed") */
  doneText?: string;
  /** a second result line (the install's note), set as soon as the result step ends */
  detail?: string;
  /** "Show details" is open (kept here: the server's frame remounts when the server goes live or stops) */
  open?: boolean;
  /** a later flow (Stop during a start) took over */
  superseded?: boolean;
}

/** What a step does: starts a TaskId command. `phase`: what the head says while it runs. */
export interface StepSpec {
  key: string;
  label: string;
  run: () => Promise<TaskId>;
  phase?: FlowPhase;
  cancel?: StepCancel;
  /** this step's result is the flow's (detail and after run as soon as it ends); default: the last step's */
  result?: boolean;
}

export interface FlowSpec<R = unknown> {
  kind: FlowKind;
  title: string;
  steps: StepSpec[];
  /** the result line when every step is done */
  done: string;
  /** a second result line from the flow's result (the step marked `result`, else the last step's) */
  detail?: (result: R) => string | undefined;
  /** called with the flow's result */
  after?: (result: R) => void;
}

const MAX_LINES = 400;
let seq = 0;

interface FlowStore {
  flows: Record<ServerId, Flow | undefined>;
  /** the spec each server's last flow ran, for Try again */
  specs: Record<ServerId, FlowSpec<any> | undefined>;
  put: (f: Flow) => void;
  dismiss: (id: ServerId) => void;
  setOpen: (id: ServerId, open: boolean) => void;
}

export const useServerFlows = create<FlowStore>((set, get) => ({
  flows: {},
  specs: {},
  put: (f) => set({ flows: { ...get().flows, [f.serverId]: f } }),
  dismiss: (id) => {
    const f = get().flows[id];
    if (!f || f.state === "running") return;
    set({ flows: { ...get().flows, [id]: undefined } });
  },
  setOpen: (id, open) => {
    const f = get().flows[id];
    if (f) set({ flows: { ...get().flows, [id]: { ...f, open } } });
  },
}));

/** What the head says while the flow runs: the running step's phase (null: no flow runs, or its step says nothing). */
export function flowPhase(f: Flow | undefined): FlowPhase | null {
  if (!f || f.state !== "running") return null;
  return f.steps.find((s) => s.state === "running")?.phase ?? null;
}

/** Ends the running step early (its `cancel`); false when it cannot be. */
export async function cancelStep(id: ServerId): Promise<boolean> {
  const f = useServerFlows.getState().flows[id];
  const st = f?.state === "running" ? f.steps.find((s) => s.state === "running") : undefined;
  if (!st?.taskId || !st.cancel) return false;
  await taskCancel(st.taskId);
  return true;
}

/** The flow of a server (undefined: none, or dismissed). */
export const useServerFlow = (id: ServerId) => useServerFlows((s) => s.flows[id]);

export const flowRunning = (id: ServerId) => useServerFlows.getState().flows[id]?.state === "running";

/** Server pages showing a flow's panel: a flow that ends while none shows it ends with a toast instead. */
const shown = new Map<ServerId, number>();
export function markShown(id: ServerId): () => void {
  shown.set(id, (shown.get(id) ?? 0) + 1);
  return () => {
    const n = (shown.get(id) ?? 1) - 1;
    if (n <= 0) shown.delete(id);
    else shown.set(id, n);
  };
}

function update(id: ServerId, s: number, f: (flow: Flow) => Flow) {
  const cur = useServerFlows.getState().flows[id];
  if (!cur || cur.seq !== s) return;
  useServerFlows.getState().put(f(cur));
}

function patchStep(id: ServerId, s: number, i: number, p: Partial<FlowStep> | ((st: FlowStep) => Partial<FlowStep>)) {
  update(id, s, (flow) => ({
    ...flow,
    steps: flow.steps.map((st, j) => (j === i ? { ...st, ...(typeof p === "function" ? p(st) : p) } : st)),
  }));
}

/** Adds the task's current message to the step's lines (when it is new) and takes its progress. */
function follow(id: ServerId, s: number, i: number, t: Task | undefined, t0: number) {
  if (!t) return;
  patchStep(id, s, i, (st) => {
    const msg = (t.state === "failed" ? t.error?.message ?? t.message : t.message)?.trim();
    const last = st.lines[st.lines.length - 1]?.text;
    const lines = msg && msg !== last ? [...st.lines, { at: Date.now() - t0, text: msg }].slice(-MAX_LINES) : st.lines;
    return lines === st.lines && st.progress === t.progress ? {} : { lines, progress: t.progress };
  });
}

export interface RunOptions {
  /** Take over from a running flow (Stop during a start): it ends as canceled and its later events are ignored. */
  supersede?: boolean;
}

/**
 * Runs a flow for a server: its steps one after another, each a task whose progress lines the flow keeps. Refused
 * (returns false) while another flow of the same server runs, unless `supersede`. Never throws: a failure is the
 * flow's state and error.
 */
export async function runFlow<R = unknown>(qc: QueryClient, id: ServerId, spec: FlowSpec<R>, opts: RunOptions = {}): Promise<boolean> {
  const prev = useServerFlows.getState().flows[id];
  if (prev?.state === "running") {
    if (!opts.supersede) return false;
    useServerFlows.getState().put({
      ...prev, state: "canceled", superseded: true, endedAt: Date.now(),
      steps: prev.steps.map((st) => (st.state === "running" ? { ...st, state: "canceled", endedAt: Date.now() } : st.state === "pending" ? { ...st, state: "skipped" } : st)),
    });
  }
  const s = ++seq;
  const t0 = Date.now();
  useServerFlows.setState((st) => ({ specs: { ...st.specs, [id]: spec } }));
  useServerFlows.getState().put({
    seq: s, kind: spec.kind, serverId: id, title: spec.title, state: "running", startedAt: t0,
    steps: spec.steps.map((x) => ({ key: x.key, label: x.label, state: "pending", progress: null, lines: [], phase: x.phase, cancel: x.cancel })),
  });
  const resultAt = spec.steps.some((x) => x.result) ? spec.steps.findIndex((x) => x.result) : spec.steps.length - 1;
  let failed: KilnError | undefined;
  let canceled = false;
  for (let i = 0; i < spec.steps.length; i++) {
    const step = spec.steps[i];
    patchStep(id, s, i, { state: "running", startedAt: Date.now() });
    let off: (() => void) | undefined;
    try {
      const taskId = await step.run();
      patchStep(id, s, i, { taskId });
      follow(id, s, i, useTasks.getState().tasks[taskId], t0);
      off = useTasks.subscribe((st, prev) => {
        if (st.tasks[taskId] !== prev.tasks[taskId]) follow(id, s, i, st.tasks[taskId], t0);
      });
      const r = await waitTask(taskId);
      follow(id, s, i, useTasks.getState().tasks[taskId], t0);
      patchStep(id, s, i, { state: "done", endedAt: Date.now(), progress: 1 });
      // the result step's note and notice at once: a later step's failure must not lose them
      if (i === resultAt && useServerFlows.getState().flows[id]?.seq === s) {
        const detail = spec.detail?.(r as R);
        if (detail) update(id, s, (flow) => ({ ...flow, detail }));
        spec.after?.(r as R);
      }
    } catch (e) {
      const k = e as KilnError;
      canceled = k?.code === "canceled";
      failed = canceled ? undefined : k ?? { code: "internal", message: String(e) };
      patchStep(id, s, i, { state: canceled ? "canceled" : "failed", endedAt: Date.now() });
      for (let j = i + 1; j < spec.steps.length; j++) patchStep(id, s, j, { state: "skipped" });
      break;
    } finally {
      off?.();
      qc.invalidateQueries({ queryKey: sk.status(id) });
    }
  }
  qc.invalidateQueries({ queryKey: sk.list });
  const mine = useServerFlows.getState().flows[id]?.seq === s;
  const ok = !failed && !canceled;
  update(id, s, (flow) => ({
    ...flow,
    state: ok ? "done" : canceled ? "canceled" : "failed",
    endedAt: Date.now(),
    error: failed,
    doneText: ok ? spec.done : undefined,
  }));
  if (mine && !shown.has(id)) {
    const detail = useServerFlows.getState().flows[id]?.detail;
    if (ok) toast.ok(spec.done, detail);
    else if (failed) toast.error(failed, spec.title);
  }
  return ok;
}

/** Try again: the server's last flow from the step that failed (the steps done before it are not done twice: a Kiln
 *  install whose start failed only starts again). */
export function retryFlow(qc: QueryClient, id: ServerId): Promise<boolean> {
  const spec = useServerFlows.getState().specs[id];
  const flow = useServerFlows.getState().flows[id];
  if (!spec) return Promise.resolve(false);
  const from = flow ? flow.steps.findIndex((st) => st.state === "failed" || st.state === "canceled") : -1;
  if (from <= 0) return runFlow(qc, id, spec);
  const steps = spec.steps.slice(from);
  // the result step ran already: its note stays with the flow that ran it
  return runFlow(qc, id, { ...spec, steps, detail: steps.some((x) => x.result) ? spec.detail : undefined, after: steps.some((x) => x.result) ? spec.after : undefined });
}

/** "1:05", "12 s" */
export function elapsed(ms: number): string {
  const s = Math.max(0, Math.floor(ms / 1000));
  if (s < 60) return `${s} s`;
  const m = Math.floor(s / 60);
  return `${m}:${String(s % 60).padStart(2, "0")}`;
}
