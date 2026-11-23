// The start's stages on Overview (ServerOverviewPage.tsx), as data: which stages a start of this server goes through,
// which one it is in, since when the start itself runs and how long is left by the last start's time. Pure, so the unit
// tests (unit/server-start.test.ts) read it without a page.
//
// The backend names the stage (Task.stage, kiln_manage::server::run): "stopping" (a restart's stop), then "preparing"
// (the folder, Kiln's setup), "wine" (only where a Wine runs the server), "process" (spawned, no log yet), "world" (the
// process writes its log), "running". A backend without stage names: read from the step's lines ("... stopped" ends a
// restart's stop, "creating the Wine prefix", "started ... (wine pid N)", then the game's lines).
import type { Task } from "../../api/commands";
import type { Flow, FlowStep } from "./serverActions";

export const START_STAGES = ["preparing", "wine", "process", "world", "running"] as const;
export type StartStage = (typeof START_STAGES)[number];
/** A stage of the track: a restart's track begins with its stop. */
export type TrackStage = "stopping" | StartStage;

/** The stages a start goes through: no Wine for a Windows server (or a Steam install the launcher starts on Windows);
 *  a remote server's `kilnctl start` over SSH says only "preparing" and "running". A restart stops first. */
export function startTrack(o: { runner?: string | null; remote?: boolean; onWindows?: boolean; restart?: boolean }): TrackStage[] {
  const stages: StartStage[] = o.remote ? ["preparing", "running"]
    : o.runner === "windows" || (o.runner === "steam" && o.onWindows) ? ["preparing", "process", "world", "running"]
    : [...START_STAGES];
  return o.restart ? ["stopping", ...stages] : stages;
}

/** The line a restart's stop ends with (run::stop: "lab stopped, world saved", "lab is not running"). */
const STOPPED = / stopped(, world saved)?$| is not running$/;

export interface StartView {
  /** the stage the start is in (one of `track`) */
  stage: TrackStage;
  /** since when the clock counts (ms since the epoch): the start itself from its first "preparing" (a restart's stop
   *  is not part of it, nor is anything before the start step); while a restart stops, the stop's own start */
  since: number;
  /** whole seconds left by the last start's time; null without one, while stopping, or once past it (`over`) */
  left: number | null;
  /** this start takes longer than the last one did */
  over: boolean;
}

/** The start step of a flow (a start's "start", a restart's "restart"). */
export const startStep = (flow: Flow | undefined): FlowStep | undefined => flow?.steps.find((s) => s.key === "start" || s.key === "restart");

/** The stage of `track` for a stage id the track may not have (an older backend's "wine" on a Windows server): the
 *  nearest one before it. */
function onTrack(id: TrackStage, track: TrackStage[]): TrackStage {
  if (track.includes(id)) return id;
  const order: TrackStage[] = ["stopping", ...START_STAGES];
  for (let i = order.indexOf(id); i >= 0; i--) if (track.includes(order[i])) return order[i];
  return track[0];
}

/**
 * Where a start is: `flow` the running start or restart (undefined: started outside the launcher, only the world is
 * known and the clock is the process's uptime), `task` its step's task, `lastStartS` the last start's time (from
 * process spawn to the world, run::await_start), `uptimeS` the process's.
 */
export function startView(flow: Flow | undefined, task: Pick<Task, "stage"> | undefined, now: number,
  o: { track: TrackStage[]; lastStartS?: number | null; uptimeS?: number | null }): StartView {
  const { track } = o;
  const last = o.lastStartS ?? null;
  const step = startStep(flow);
  let stage: TrackStage;
  let since: number;
  if (!flow || !step) {
    stage = onTrack("world", track);
    since = o.uptimeS != null ? now - o.uptimeS * 1000 : now;
  } else {
    const t0 = flow.startedAt;
    const stepAt = step.startedAt ?? t0;
    const named = (step.stages ?? []).filter((s) => s.id === "stopping" || (START_STAGES as readonly string[]).includes(s.id));
    const current = (task?.stage ?? named[named.length - 1]?.id) as TrackStage | undefined;
    if (current && (current === "stopping" || (START_STAGES as readonly string[]).includes(current))) {
      stage = onTrack(current, track);
      const begun = named.find((s) => s.id !== "stopping");
      since = current === "stopping" ? stepAt : begun ? t0 + begun.at : stepAt;
    } else {
      // no stage names: the lines say it
      const lines = step.lines;
      let from = 0;
      if (flow.kind === "restart") {
        const stopped = lines.findIndex((l) => STOPPED.test(l.text));
        from = stopped + 1;
        if (stopped < 0) from = -1;
      }
      if (from < 0) {
        stage = onTrack("stopping", track);
        since = stepAt;
      } else {
        const rest = lines.slice(from).map((l) => l.text);
        const started = rest.findIndex((l) => /^started /i.test(l));
        const id: StartStage = started >= 0 ? (started < rest.length - 1 ? "world" : "process") : rest.some((l) => /wine prefix|wineboot/i.test(l)) ? "wine" : "preparing";
        stage = onTrack(id, track);
        // a restart's start begins with the line after its stop's last one (the first line of the start, or now)
        since = flow.kind === "restart" ? (from < lines.length ? t0 + lines[from].at : from > 0 ? t0 + lines[from - 1].at : stepAt) : stepAt;
      }
    }
  }
  if (stage === "stopping" || last == null) return { stage, since, left: null, over: false };
  const took = Math.max(0, now - since) / 1000;
  return took >= last ? { stage, since, left: null, over: true } : { stage, since, left: Math.max(1, Math.ceil(last - took)), over: false };
}
