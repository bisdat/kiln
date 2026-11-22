// Server actions with feedback. Every Start, Stop, Restart, game update and Kiln install runs as a flow: one or more
// task steps (for an update of a running server: stop, install, start again). The store keeps, per server, the flow's
// steps, each step's task and the distinct progress lines the task reported, so the server's head shows what happens
// now, how long it has taken and, at the end, the result or the reason it failed. The status query polls every second
// while a flow runs or the server is starting or stopping (useServerStatus).
import type { QueryClient } from "@tanstack/react-query";
import { create } from "zustand";
import type { KilnError, ServerId, ServerStatus, Task, TaskId } from "../../api/commands";
import { toast, useTasks, waitTask } from "../../ui";
import { sk } from "./serverApi";

export type StepState = "pending" | "running" | "done" | "failed" | "canceled" | "skipped";
export type FlowState = "running" | "done" | "failed" | "canceled";
export type FlowKind = "start" | "stop" | "restart" | "update" | "deploy";

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
  /** a second result line (the install's note) */
  detail?: string;
}

/** What a step does: starts a TaskId command. `optimistic` is the state the head shows at once (the next status read
 *  replaces it). */
export interface StepSpec {
  key: string;
  label: string;
  run: () => Promise<TaskId>;
  optimistic?: ServerStatus["state"];
  /** this step's result is the flow's (detail, after); default: the last step's */
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
}));

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

function optimistic(qc: QueryClient, id: ServerId, state: ServerStatus["state"] | undefined) {
  if (!state) return;
  qc.setQueryData<ServerStatus>(sk.status(id), (old) => (old ? { ...old, state } : old));
}

/**
 * Runs a flow for a server: its steps one after another, each a task whose progress lines the flow keeps. Refused
 * (returns false) while another flow of the same server runs. Never throws: a failure is the flow's state and error.
 */
export async function runFlow<R = unknown>(qc: QueryClient, id: ServerId, spec: FlowSpec<R>): Promise<boolean> {
  if (flowRunning(id)) return false;
  const s = ++seq;
  const t0 = Date.now();
  useServerFlows.setState((st) => ({ specs: { ...st.specs, [id]: spec } }));
  useServerFlows.getState().put({
    seq: s, kind: spec.kind, serverId: id, title: spec.title, state: "running", startedAt: t0,
    steps: spec.steps.map((x) => ({ key: x.key, label: x.label, state: "pending", progress: null, lines: [] })),
  });
  let result: unknown = null;
  let failed: KilnError | undefined;
  let canceled = false;
  for (let i = 0; i < spec.steps.length; i++) {
    const step = spec.steps[i];
    patchStep(id, s, i, { state: "running", startedAt: Date.now() });
    optimistic(qc, id, step.optimistic);
    let off: (() => void) | undefined;
    try {
      const taskId = await step.run();
      patchStep(id, s, i, { taskId });
      follow(id, s, i, useTasks.getState().tasks[taskId], t0);
      off = useTasks.subscribe((st, prev) => {
        if (st.tasks[taskId] !== prev.tasks[taskId]) follow(id, s, i, st.tasks[taskId], t0);
      });
      const r = await waitTask(taskId);
      if (step.result || (i === spec.steps.length - 1 && !spec.steps.some((x) => x.result))) result = r;
      follow(id, s, i, useTasks.getState().tasks[taskId], t0);
      patchStep(id, s, i, { state: "done", endedAt: Date.now(), progress: 1 });
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
  const ok = !failed && !canceled;
  const detail = ok ? spec.detail?.(result as R) : undefined;
  update(id, s, (flow) => ({
    ...flow,
    state: ok ? "done" : canceled ? "canceled" : "failed",
    endedAt: Date.now(),
    error: failed,
    doneText: ok ? spec.done : undefined,
    detail,
  }));
  if (ok) spec.after?.(result as R);
  if (!shown.has(id)) {
    if (ok) toast.ok(spec.done, detail);
    else if (failed) toast.error(failed, spec.title);
  }
  return ok;
}

/** Runs the server's last flow again (Try again). */
export function retryFlow(qc: QueryClient, id: ServerId): Promise<boolean> {
  const spec = useServerFlows.getState().specs[id];
  return spec ? runFlow(qc, id, spec) : Promise.resolve(false);
}

/** "1:05", "12 s" */
export function elapsed(ms: number): string {
  const s = Math.max(0, Math.floor(ms / 1000));
  if (s < 60) return `${s} s`;
  const m = Math.floor(s / 60);
  return `${m}:${String(s % 60).padStart(2, "0")}`;
}
