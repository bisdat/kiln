// /servers/:id/overview (the Servers redesign, mockup A). While the server starts: the start's five stages as cones
// (preparing, Wine, server process, loading the world, running), the clock, "about N s left" from the last start, the
// newest line, the console, and what can still be done meanwhile. Otherwise: the Kiln update when a newer runtime is
// here, the numbers that matter (players, health, uptime, versions), one line about each section, who is online and
// the console. Kiln missing from the folder: the Install Kiln banner first.
import { useQuery } from "@tanstack/react-query";
import { useEffect, useState } from "react";
import { NavLink, useParams } from "react-router-dom";
import { useAdminState, useInAdminProvider } from "../../api/admin";
import * as api from "../../api/commands";
import type { Task } from "../../api/commands";
import { Banner, Button, Cone, KeyValue, Sparkline, fmtAgo, useTasks } from "../../ui";
import { useTicks } from "../ticks";
import { ServerConsole } from "./ServerConsole";
import { elapsed, useServerFlow, type Flow } from "./serverActions";
import { hasKiln, kilnUpdateFor, runnerLabel, sk, useDeployDialog, useServer, useServerStatus, type ServerModX } from "./serverApi";
import { useT } from "../../i18n";

export const START_STAGES = ["preparing", "wine", "process", "world", "running"] as const;
export type StartStage = (typeof START_STAGES)[number];

/** Which stage a start is in: the task's own stage when the backend names it (Task.stage), else read from its lines
 *  ("creating the Wine prefix", "started ... (wine pid N)", then the game's log lines). -1: not a start. */
export function startStage(flow: Flow | undefined, task: (Task & { stage?: string | null }) | undefined): number {
  if (!flow) return -1;
  if (flow.state === "done") return START_STAGES.length - 1;
  const step = flow.steps.find((s) => s.key === "start" || s.key === "restart");
  if (!step || step.state === "pending") return -1;
  const named = task?.stage ? START_STAGES.indexOf(task.stage as StartStage) : -1;
  if (named >= 0) return named;
  const lines = step.lines.map((l) => l.text);
  const started = lines.findIndex((l) => /^started /i.test(l));
  if (started >= 0) return started < lines.length - 1 ? 3 : 2;
  if (lines.some((l) => /wine prefix|wineboot/i.test(l))) return 1;
  return 0;
}

export default function ServerOverviewPage() {
  const { id = "" } = useParams();
  const st = useServerStatus(id);
  const flow = useServerFlow(id);
  const startFlow = flow && (flow.kind === "start" || flow.kind === "restart") && flow.state === "running" ? flow : undefined;
  if (startFlow || st.data?.state === "starting") return <Starting id={id} flow={startFlow} />;
  return <Steady id={id} />;
}

function useNow(on: boolean) {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (!on) return;
    const h = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(h);
  }, [on]);
  return now;
}

function Starting({ id, flow }: { id: string; flow: Flow | undefined }) {
  const t = useT();
  const st = useServerStatus(id);
  const srv = useServer(id);
  const now = useNow(true);
  const step = flow?.steps.find((s) => s.key === "start" || s.key === "restart");
  const task = useTasks((s) => (step?.taskId ? s.tasks[step.taskId] : undefined)) as (Task & { stage?: string | null }) | undefined;
  // started outside the launcher (kilnctl, another launcher): only "loading the world" is known
  const at = flow ? Math.max(0, startStage(flow, task)) : 3;
  const since = flow?.startedAt ?? (st.data?.uptimeS != null ? now - st.data.uptimeS * 1000 : now);
  const took = now - since;
  const last = (st.data as { lastStartS?: number | null } | undefined)?.lastStartS ?? null;
  const left = last != null ? Math.max(0, Math.round(last - took / 1000)) : null;
  const newest = step?.lines[step.lines.length - 1]?.text;
  return (
    <div className="k-sov k-sov-start">
      <div className="k-sov-live">
        <ol className="k-strack" aria-label={t("servers.overview.stages")}>
          {START_STAGES.map((s, i) => (
            <li key={s} className={i < at ? "done" : i === at ? "now" : ""} aria-current={i === at ? "step" : undefined}>
              <Cone state={i < at ? "done" : i === at ? "bend" : ""} />
              <b>{t(`servers.overview.stage.${s}`)}</b>
              <span className="d">{t(`servers.overview.stageHint.${s}`)}</span>
            </li>
          ))}
        </ol>
        <div className="k-sclockrow">
          <span className="k-sclock" aria-label={t("servers.overview.elapsed")}>{elapsed(took)}</span>
          <span className="k-sclock-lab">
            <b>{t("servers.overview.elapsed")}</b>
            <span>{left != null ? t("servers.overview.left", { left: elapsed(left * 1000), last: elapsed(last! * 1000) }) : t("servers.overview.noLast")}</span>
          </span>
        </div>
        {newest && <div className="k-slastline" title={newest}><span>{newest}</span></div>}
        <ServerConsole serverId={id} />
      </div>
      <aside className="k-sov-aside" aria-label={t("servers.overview.whileStarts")}>
        <div className="k-section-title">{t("servers.overview.whileStarts")}</div>
        <ul className="k-scan">
          <li><b>{t("servers.overview.can.watch")}</b><span>{t("servers.overview.can.watchHint")}</span></li>
          <li><b>{t("servers.overview.can.leave")}</b><span>{t("servers.overview.can.leaveHint")}</span></li>
          <li><b>{t("servers.overview.can.stop")}</b><span>{t("servers.overview.can.stopHint")}</span></li>
          <li className="no"><b>{t("servers.overview.can.join")}</b><span>{t("servers.overview.can.joinHint")}</span></li>
        </ul>
        <KeyValue rows={[
          srv.data && [t("servers.overview.runBy"), runnerLabel(srv.data.runner ?? "kilnctl")],
          srv.data && [t("servers.overview.port"), <span className="k-mono">UDP {srv.data.queryPort}</span>],
          (st.data?.installed || st.data?.kiln) && ["Kiln", <span className="k-mono">{st.data?.kiln ?? st.data?.installed}</span>],
        ]} />
      </aside>
    </div>
  );
}

function Steady({ id }: { id: string }) {
  const t = useT();
  const srv = useServer(id);
  const st = useServerStatus(id);
  const s = srv.data;
  const status = st.data;
  const running = status?.state === "running";
  const query = useQuery({ queryKey: sk.query(id), queryFn: () => api.serverQuery(id), enabled: running, refetchInterval: 15000, retry: false });
  const bundles = useQuery({ queryKey: ["runtime_list"], queryFn: api.runtimeList, staleTime: 60_000 });
  const mods = useQuery({ queryKey: sk.mods(id), queryFn: () => api.serverMods(id) as Promise<ServerModX[]>, retry: false, staleTime: 15_000 });
  const backups = useQuery({ queryKey: sk.backups(id), queryFn: () => api.serverBackups(id), retry: false, staleTime: 30_000 });
  const { ticks } = useTicks({ server: id });
  const showDeploy = useDeployDialog((d) => d.show);
  const flow = useServerFlow(id);
  const busy = flow?.state === "running" || status?.state === "stopping";
  const noKiln = !!status && !hasKiln(s, status);
  const remote = s?.runner === "remote" || s?.kind === "remote";
  const update = !remote && status ? kilnUpdateFor(status.installed ?? null, bundles.data ?? []) : null;
  const recent = ticks.slice(-16);
  const last = recent[recent.length - 1];
  const players = status?.players ?? query.data?.players ?? null;
  const max = query.data?.maxPlayers ?? null;
  const lastStart = (status as { lastStartS?: number | null } | undefined)?.lastStartS ?? null;
  const base = `/servers/${encodeURIComponent(id)}`;
  const newestBackup = (backups.data ?? []).map((b) => Date.parse(b.created)).filter((n) => !Number.isNaN(n)).sort((a, b) => b - a)[0];
  return (
    <div className="k-sov">
      {noKiln && (
        <Banner tone="idle" title={t("servers.overview.noKiln")}
          actions={<Button size="s" disabled={busy} onClick={() => showDeploy(true)}>{t("servers.head.installKiln")}</Button>}>
          {status?.pid ? t("servers.activity.installStopsFirst") : null}
        </Banner>
      )}
      {update && !noKiln && (
        <Banner tone="warn" title={t("servers.overview.kilnUpdate")}
          actions={<Button size="s" variant="primary" disabled={busy} onClick={() => showDeploy(true)}>{t("servers.head.updateKiln")}</Button>}>
          {running ? t("servers.overview.kilnUpdateRunning", { latest: update.latest, current: update.current }) : t("servers.overview.kilnUpdateStopped", { latest: update.latest, current: update.current })}
        </Banner>
      )}
      <dl className="k-sstats">
        <div className="k-sstat">
          <dt>{t("servers.overview.stat.players")}</dt>
          <dd>{running ? <OnlineCount fallback={players} /> : "-"}{max != null && <small> / {max}</small>}</dd>
          <p>{running ? <OnlineNames fallback={players ? t("servers.head.online", { count: players }) : t("servers.head.nobody")} /> : t("servers.overview.stat.notRunning")}</p>
        </div>
        <div className="k-sstat">
          <dt>{t("servers.overview.stat.health")}</dt>
          <dd>{last ? last.rate.toFixed(1) : "-"}<small> {t("servers.overview.stat.stepsPerS")}</small>{recent.length > 1 && <Sparkline values={recent.map((x) => x.rate)} width={90} height={24} aria-label={t("servers.overview.stat.health")} />}</dd>
          <p>{last ? t("servers.overview.stat.p99", { ms: last.step_ms.p99.toFixed(1) }) : t("servers.overview.stat.noTicks")}</p>
        </div>
        <div className="k-sstat">
          <dt>{t("servers.overview.stat.uptime")}</dt>
          <dd>{running && status?.uptimeS != null ? elapsed(status.uptimeS * 1000) : "-"}</dd>
          <p>{lastStart != null ? t("servers.overview.stat.startedIn", { time: elapsed(lastStart * 1000) }) : running ? t("servers.list.running") : t("servers.list.stopped")}</p>
        </div>
        <div className="k-sstat">
          <dt>{t("servers.overview.stat.versions")}</dt>
          <dd className="mono">{status?.kiln ?? status?.installed ?? t("servers.overview.stat.noKiln")}</dd>
          <p>{update ? <span className="k-tone-warn">{t("servers.overview.stat.isOut", { latest: update.latest })}</span> : null}{query.data?.version ? ` ${t("servers.overview.stat.game", { version: query.data.version })}` : ""}</p>
        </div>
      </dl>
      <nav className="k-slinks" aria-label={t("servers.overview.sections")}>
        <NavLink className="k-slink" to={`${base}/console`}><b>{t("shell.serverNav.console")}</b><span>{running ? t("servers.overview.link.consoleLive") : t("servers.overview.link.consoleLast")}</span></NavLink>
        <NavLink className="k-slink" to={`${base}/admin/players`}><b>{t("shell.serverNav.players")}</b><span>{players != null ? t("servers.head.online", { count: players }) : t("servers.overview.link.playersOff")}</span></NavLink>
        <NavLink className="k-slink" to={`${base}/mods`}><b>{t("shell.serverNav.mods")}</b><span>{mods.data ? t("servers.overview.link.mods", { count: mods.data.length }) : "…"}</span></NavLink>
        <NavLink className="k-slink" to={`${base}/backups`}><b>{t("shell.serverNav.backups")}</b><span>{backups.data ? (newestBackup ? t("servers.overview.link.backups", { count: backups.data.length, ago: fmtAgo(newestBackup) }) : t("servers.overview.link.noBackups")) : "…"}</span></NavLink>
        <NavLink className="k-slink" to={`${base}/settings`}><b>{t("shell.serverNav.settings")}</b><span>{s ? t("servers.overview.link.settings", { port: s.queryPort }) : "…"}</span></NavLink>
      </nav>
      <div className="k-sov-console">
        <ServerConsole serverId={id} />
      </div>
    </div>
  );
}

/** How many are online: Kiln.Admin's list when it is connected, else the status / query count. */
function OnlineCount({ fallback }: { fallback: number | null }) {
  if (!useInAdminProvider()) return <>{fallback ?? "-"}</>;
  return <AdminCount fallback={fallback} />;
}
function AdminCount({ fallback }: { fallback: number | null }) {
  const st = useAdminState();
  return <>{st.state?.players?.length ?? fallback ?? "-"}</>;
}

/** The names of who is online, from Kiln.Admin when it is connected. */
function OnlineNames({ fallback }: { fallback: string }) {
  if (!useInAdminProvider()) return <>{fallback}</>;
  return <AdminNames fallback={fallback} />;
}
function AdminNames({ fallback }: { fallback: string }) {
  const st = useAdminState();
  const names = (st.state?.players ?? []).map((p: { name?: string }) => p.name).filter(Boolean) as string[];
  return <>{names.length ? names.slice(0, 4).join(", ") + (names.length > 4 ? ` +${names.length - 4}` : "") : fallback}</>;
}
