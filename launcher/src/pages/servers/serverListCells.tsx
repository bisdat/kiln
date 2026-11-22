// The Servers list's cells (ServersPage.tsx): a server's state (or the flow that runs on it, live: the word, the cones,
// the running step and its time, the idea taken from mockup B), its players, its health (steps per second from the
// tick stats), its Kiln and the Update word, where it runs, and its quick actions with the ··· menu.
import { MoreHorizontal } from "lucide-react";
import type { ServerStatus } from "../../api/commands";
import { Badge, Button, Cones, Menu, Sparkline, type MenuItem, type Tone } from "../../ui";
import { useT, type TFunction } from "../../i18n";
import { runnerLabel, type ServerInstanceX } from "./serverApi";
import { elapsed, type Flow, type FlowStep } from "./serverActions";
import { fmtClock, fmtDuration } from "./activityLog";
import { shortKiln } from "./kilnUpdate";

export interface ServerRow {
  srv: ServerInstanceX;
  st: ServerStatus | undefined;
  /** the server's flow (running, or the last one until it is dismissed) */
  flow: Flow | undefined;
  remote: boolean;
  /** the Kiln version an update installs (kilnUpdate.ts), or null */
  update: string | null;
  /** the Kiln in its folder (a remote server: the Kiln it runs) */
  kiln: string | null;
  maxPlayers: number | null;
  /** steps per second, one a minute, oldest first (running local servers only) */
  rates: number[];
}

export interface RowActions {
  start: (r: ServerRow) => void;
  stop: (r: ServerRow) => void;
  restart: (r: ServerRow) => void;
  backup: (r: ServerRow) => void;
  kiln: (r: ServerRow) => void;
  console: (r: ServerRow) => void;
  remove: (r: ServerRow) => void;
}

/** The word of a running flow: the running step's (Starting, Stopping, Updating), else the action's. */
export function flowWord(f: Flow, step: FlowStep | undefined, t: TFunction): string {
  switch (step?.phase) {
    case "starting": return f.kind === "restart" ? t("servers.list.word.restarting") : t("servers.list.word.starting");
    case "stopping": return f.kind === "restart" ? t("servers.list.word.restarting") : t("servers.list.word.stopping");
    case "updating": return f.kind === "create" ? t("servers.list.word.installing") : t("servers.list.word.updating");
  }
  switch (f.kind) {
    case "create": return t("servers.list.word.installing");
    case "backup": return t("servers.list.word.backingUp");
    case "restart": return t("servers.list.word.restarting");
    case "start": return t("servers.list.word.starting");
    case "stop": return t("servers.list.word.stopping");
    default: return t("servers.list.word.updating");
  }
}

function stateWord(state: ServerStatus["state"] | undefined, t: TFunction): string {
  switch (state) {
    case "running": return t("servers.list.running");
    case "starting": return t("servers.list.starting");
    case "stopping": return t("servers.list.stopping");
    case "stopped": return t("servers.list.stopped");
    case undefined: return "…";
    default: return state.charAt(0).toUpperCase() + state.slice(1);
  }
}

/** State: the live flow while one runs, else the server's word and how long (running for, stopped since). */
export function StateCell({ row, now, since }: { row: ServerRow; now: number; since: number | null }) {
  const t = useT();
  const { flow, st } = row;
  if (flow?.state === "running") {
    const i = flow.steps.findIndex((s) => s.state === "running");
    const step = i >= 0 ? flow.steps[i] : undefined;
    const many = flow.steps.length > 1;
    const label = step ? (many ? t("servers.list.stepOf", { n: i + 1, count: flow.steps.length, step: step.label }) : step.label) : flow.title;
    const line = step?.lines[step.lines.length - 1]?.text;
    return (
      <span className="k-sact-cell k-srow-live" title={line ?? flow.title} data-flow={flow.kind}>
        <span className="k-srow-live-top">
          <Badge tone="ember">{flowWord(flow, step, t)}</Badge>
          <span className="k-srow-cones" style={{ ["--n" as string]: many ? flow.steps.length : 3 }}>
            <Cones step={many ? Math.max(0, i) : Math.floor(now / 900) % 3} count={many ? flow.steps.length : 3} small />
          </span>
          <span className="k-mono k-muted k-srow-time">{elapsed(now - (step?.startedAt ?? flow.startedAt))}</span>
        </span>
        <span className="k-srow-live-step">{label}</span>
      </span>
    );
  }
  const state = st?.state;
  const tone: Tone = state === "running" ? "ok" : state === "starting" || state === "stopping" ? "warn" : "idle";
  const sub = state === "running" && st?.uptimeS != null ? fmtDuration(st.uptimeS * 1000, t)
    : state === "stopped" && since ? t("servers.list.since", { time: fmtClock(since, now) }) : null;
  // a failed action: the server's own state, and a marker naming the failure
  const failed = flow?.state === "failed" ? (
    <span className="k-badge k-tone-err" title={flow.error?.message ?? ""} aria-label={t("servers.activity.failedMarker", { reason: flow.error?.message ?? "" })}>!</span>
  ) : null;
  return (
    <span className="k-sact-cell">
      <Badge tone={tone}>{stateWord(state, t)}</Badge>
      {sub && <span className="k-muted k-srow-sub">{sub}</span>}
      {failed}
    </span>
  );
}

export function OnlineCell({ row }: { row: ServerRow }) {
  const p = row.st?.state === "running" ? row.st.players : null;
  if (p == null) return <span className="k-muted">-</span>;
  return <span className="k-mono k-srow-num">{row.maxPlayers != null ? `${p} / ${row.maxPlayers}` : p}</span>;
}

/** Steps per second (the tick stats' last minutes), only for a running server on this PC. */
export function HealthCell({ row }: { row: ServerRow }) {
  const t = useT();
  if (row.st?.state !== "running" || row.remote || !row.rates.length) return <span className="k-muted">-</span>;
  const last = row.rates[row.rates.length - 1];
  const rate = last.toFixed(1);
  return (
    <span className="k-srow-health" title={t("servers.list.healthTitle", { rate, count: row.rates.length })}>
      {row.rates.length > 1 && <Sparkline values={row.rates} width={54} height={20} aria-label={t("servers.list.healthLabel", { rate, count: row.rates.length })} />}
      <span className="k-mono">{t("servers.list.perSecond", { rate })}</span>
    </span>
  );
}

export function KilnCell({ row }: { row: ServerRow }) {
  const t = useT();
  if (!row.st) return null;
  if (!row.kiln) return <span className="k-muted">-</span>;
  return (
    <span className="k-srow-kiln">
      <span className="k-mono" title={row.kiln}>{shortKiln(row.kiln)}</span>
      {row.update && <Badge tone="warn" title={t("servers.list.updateTitle", { version: row.update })}>{t("servers.list.updateWord")}</Badge>}
    </span>
  );
}

export function RunsOnCell({ srv }: { srv: ServerInstanceX }) {
  const t = useT();
  const host = srv.remote?.host;
  const text = (srv.runner === "remote" || srv.kind === "remote") && host ? t("servers.list.overSsh", { host })
    : (srv.source as string) === "steam" ? t("servers.list.foundInSteam", { runner: runnerLabel(srv.runner ?? "kilnctl") })
    : runnerLabel(srv.runner ?? (srv.kind === "remote" ? "remote" : "kilnctl"));
  return <span className="k-muted k-srow-runs" title={text}>{text}</span>;
}

/** Quick actions: Update Kiln when there is one (else Console while it runs), Start or Stop, and the ··· menu. Clicks
 *  stay here (the row opens the server); the row's keys ignore keys from its buttons (ui/Table.tsx). */
export function ActionsCell({ row, act, target }: { row: ServerRow; act: RowActions; target: string | null }) {
  const t = useT();
  const { srv, st, flow } = row;
  const name = srv.name;
  const busy = flow?.state === "running" ? flow.kind : null;
  const state = st?.state;
  const moving = state === "starting" || state === "stopping";
  const up = busy === "start" || state === "running" || state === "starting";
  // a process here, or a remote server that answers (a remote status has no pid: kilnctl runs it over there)
  const canStop = busy === "start" || (!busy && (!!st?.pid || (row.remote && state === "running")));
  const canStart = !busy && !moving && !!st && !st.pid;
  const canKiln = !row.remote && !busy && !moving && !!st && !!target && (!!row.update || !row.kiln);
  const quick = row.update && !row.remote ? (
    <Button size="s" variant="ghost" disabled={!canKiln} onClick={() => act.kiln(row)} aria-label={t("servers.list.updateNamed", { name })}>{t("servers.head.updateKiln")}</Button>
  ) : state === "running" && !row.remote ? (
    <Button size="s" variant="ghost" onClick={() => act.console(row)} aria-label={t("servers.list.consoleNamed", { name })}>{t("servers.list.console")}</Button>
  ) : null;
  const items: MenuItem[] = [
    { label: t("servers.head.restart"), disabled: !!busy || state !== "running", onSelect: () => act.restart(row) },
    { label: t("servers.activity.console"), onSelect: () => act.console(row) },
    { label: t("servers.backups.now"), disabled: !!busy || moving, onSelect: () => act.backup(row) },
    ...(!row.remote ? [{
      label: <>{row.kiln ? t("servers.head.updateKiln") : t("servers.head.installKiln")}{canKiln && target && <span className="k-mono">{shortKiln(target)}</span>}</>,
      sep: true, disabled: !canKiln, onSelect: () => act.kiln(row),
    }] : []),
    { label: t("servers.head.removeMenu"), danger: true, sep: true, disabled: !!st?.pid || !!busy, onSelect: () => act.remove(row) },
  ];
  return (
    <span className="k-srow-actions" onClick={(e) => e.stopPropagation()}>
      {quick}
      {up ? (
        <Button size="s" variant="danger" disabled={!canStop} onClick={() => act.stop(row)} aria-label={t("servers.list.stopNamed", { name })}>{t("servers.head.stop")}</Button>
      ) : (
        <Button size="s" disabled={!canStart} onClick={() => act.start(row)} aria-label={t("servers.list.startNamed", { name })}>{t("servers.head.start")}</Button>
      )}
      <Menu label={t("servers.list.moreFor", { name })} icon={<MoreHorizontal />} items={items} size="s" />
    </span>
  );
}
