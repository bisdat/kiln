// /servers/:id/*: one grouped list (Run, World, Data, Files) beside the server's name, its live sentence (state,
// players, the admin connection) and Start/Restart/Stop; then the page. Wraps the admin pages too
// (/servers/:id/admin/*). Server actions that need no room (update the game, install Kiln, reconnect, remove) sit in a
// labelled menu beside the buttons.
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { MoreHorizontal } from "lucide-react";
import { useEffect, useRef, useState, type KeyboardEvent as ReactKeyboardEvent, type ReactNode } from "react";
import { Link, NavLink, Outlet, useLocation, useNavigate, useParams } from "react-router-dom";
import { AdminProvider, reconnectAdmin, useAdminState } from "../../api/admin";
import * as api from "../../api/commands";
import type { AdminTransport } from "../../api/commands";
import { Button, ConfirmDanger, Field, Menu, Modal, Select, Skeleton, Toggle, toast, type MenuItem } from "../../ui";
import { SERVER_SECTIONS, serverPlace } from "../../shell/serverNav";
import { ConfirmHost } from "../../admin/ui";
import { hasKiln, runnerLabel, sk, useDeployDialog, useServer, useServerEvents, useServers, useServerStatus } from "./serverApi";
import { flowPhase, runFlow, useServerFlow } from "./serverActions";
import { flowWord } from "./serverListCells";
import { kilnFlow, restartFlow, startFlow, stepStart, stepStop, stopServer } from "./serverFlows";
import { ServerActivity } from "./ServerActivity";
import { rich, t, useT } from "../../i18n";

export default function ServerLayout() {
  const { id = "" } = useParams();
  useServerEvents();
  const st = useServerStatus(id);
  const live = st.data?.state === "running" || st.data?.state === "starting";
  const body = <ServerFrame id={id} live={live} />;
  // the shared admin session only while the server runs (no reconnect loop for a stopped one)
  return live ? <AdminProvider serverId={id}>{body}</AdminProvider> : body;
}

/** The server's frame (mockup A): the way back and the other servers, the head (name, state word, one sentence,
 *  actions), the seven section tabs and a section's own pages, the activity panel, then the page. */
function ServerFrame({ id, live }: { id: string; live: boolean }) {
  const t = useT();
  const loc = useLocation();
  const rest = loc.pathname.replace(/^\/servers\/[^/]+\/?/, "");
  const place = serverPlace(rest);
  const st = useServerStatus(id);
  const base = `/servers/${encodeURIComponent(id)}`;
  const badge = (sec: string) => {
    if (sec === "console" && (st.data?.state === "running" || st.data?.state === "starting")) return <small className="hot">{t("servers.frame.live")}</small>;
    if (sec === "players" && st.data?.players) return <small className="hot">{st.data.players}</small>;
    return null;
  };
  return (
    <div className="k-page k-page-fill k-server">
      <ServerTop id={id} />
      <ServerHead id={id} live={live} />
      {/* plain links with the state worked out here: a NavLink drops aria-current on a section's other pages (its own
          link is not the page then), and the section tab must keep it there */}
      <nav className="k-stabs" aria-label={t("servers.head.navLabel")}>
        {SERVER_SECTIONS.map((sec) => {
          const on = place?.section.id === sec.id;
          return (
            <Link key={sec.id} to={`${base}/${sec.tabs[0].to}`} className={"k-stab" + (on ? " on" : "")} aria-current={on ? "page" : undefined}>
              {sec.label}{badge(sec.id)}
            </Link>
          );
        })}
      </nav>
      {place && place.section.tabs.length > 1 && (
        <nav className="k-stabs-sub" aria-label={place.section.label}>
          {place.section.tabs.map((i) => (
            <Link key={i.to} to={`${base}/${i.to}`} className={"k-stab-sub" + (place.tab.to === i.to ? " on" : "")} aria-current={place.tab.to === i.to ? "page" : undefined}>
              {i.label}
            </Link>
          ))}
        </nav>
      )}
      {/* a running start shows its stages on Overview itself; every other action, a start that ended (failed: its reason
          and Try again) and a start seen from another page show here */}
      <ServerActivity id={id} hideKinds={place?.section.id === "overview" ? ["start", "restart"] : undefined} />
      <div className="k-server-page">
        <Outlet />
      </div>
      <ConfirmHost />
    </div>
  );
}

/** "‹ All servers" and every server with its state's dot and word (a click opens it, on the same section). */
function ServerTop({ id }: { id: string }) {
  const t = useT();
  const servers = useServers();
  const loc = useLocation();
  const rest = loc.pathname.replace(/^\/servers\/[^/]+\/?/, "") || "overview";
  const list = servers.data ?? [];
  return (
    <div className="k-stop">
      <NavLink className="k-link k-scrumb" to="/servers">{t("servers.frame.allServers")}</NavLink>
      {list.length > 1 && (
        <nav className="k-sswitch" aria-label={t("servers.frame.switch")}>
          {list.map((s) => <SwitchDot key={s.id} id={s.id} name={s.name} current={s.id === id} to={`/servers/${encodeURIComponent(s.id)}/${rest}`} />)}
        </nav>
      )}
    </div>
  );
}

/** How often the switcher reads the other servers' status: a dot is no page (the current server's is the page's own). */
const OTHERS_EVERY = 30_000;

function SwitchDot({ id, name, current, to }: { id: string; name: string; current: boolean; to: string }) {
  const t = useT();
  const st = useServerStatus(id, current ? undefined : OTHERS_EVERY);
  const flow = useServerFlow(id);
  const s = st.data?.state;
  const tone = flow?.state === "running" || s === "starting" || s === "stopping" ? "busy" : s === "running" ? "ok" : "idle";
  // the dot's meaning in words, for screen readers (and a pointer's tooltip)
  const word = flow?.state === "running" ? flowWord(flow, flow.steps.find((x) => x.state === "running"), t)
    : s === "running" ? t("servers.list.running") : s === "starting" ? t("servers.list.starting") : s === "stopping" ? t("servers.list.stopping")
    : s === "stopped" ? t("servers.list.stopped") : s ? t("servers.frame.unknown") : "…";
  return (
    <Link to={to} className={"k-sswitch-item" + (current ? " on" : "")} aria-current={current ? "page" : undefined}
      aria-label={t("servers.frame.switchItem", { name, state: word })} title={word}>
      <i className={tone} aria-hidden />{name}
    </Link>
  );
}

function dur(s: number | null | undefined): string {
  if (s == null) return "";
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return h ? t("servers.head.durH", { h, m }) : m ? t("servers.head.durM", { m, s: sec }) : t("servers.head.durS", { s: sec });
}

/** "2 online, 2 slots free." / "Nobody online." (no leading space). */
function whoLine(n: number, max: number | null, free: number): string {
  const who = n === 0 ? t("servers.head.nobody") : t("servers.head.online", { count: n });
  if (max == null) return n === 0 ? t("servers.head.nobodyLine") : t("servers.head.onlineLine", { count: n });
  return t("servers.head.slotsFree", { who, count: free });
}

function ServerHead({ id, live }: { id: string; live: boolean }) {
  const t = useT();
  const qc = useQueryClient();
  const nav = useNavigate();
  const srv = useServer(id);
  const st = useServerStatus(id);
  const servers = useServers();
  const query = useQuery({ queryKey: sk.query(id), queryFn: () => api.serverQuery(id), enabled: st.data?.state === "running", refetchInterval: 15000, retry: false });
  const flow = useServerFlow(id);
  const busy = flow?.state === "running" ? flow.kind : null;
  // the running step's word (starting, stopping, updating) before the backend's: the status keeps the backend's own
  const phase = flowPhase(flow);
  const { open: deploy, show: setDeploy } = useDeployDialog();
  const [removing, setRemoving] = useState(false);
  const [pick, setPick] = useState(false);
  const state = st.data?.state;
  const s = srv.data;
  const name = s?.name ?? id;
  const start = () => void runFlow(qc, id, startFlow(id, name));
  // Stop also ends a start that is still waiting for the world: the start's wait ends, the stop takes over
  const stop = () => void stopServer(qc, id, name);
  const restart = () => void runFlow(qc, id, restartFlow(id, name));
  const pid = !!st.data?.pid;
  const updateGame = () => void runFlow(qc, id, { kind: "update", title: t("servers.activity.updateGameTitle", { name }),
    steps: [...(pid ? [stepStop(id)] : []), { key: "update", label: t("servers.activity.stepUpdateGame"), run: () => api.serverUpdate(id), phase: "updating", cancel: "cancel" }, ...(pid ? [stepStart(id, true)] : [])],
    done: t("servers.head.upToDate") });
  // through the shared flow, so Recent says "updated from X to Y"
  const deployKiln = (version: string, startAfter: boolean) =>
    void runFlow<api.InstallResult>(qc, id, kilnFlow({ id, name, version, running: pid, startAfter, from: kilnInstalledNow ? (st.data?.installed ?? st.data?.kiln ?? null) : null }));
  // the dialog belongs to this server: leaving it closes the dialog
  useEffect(() => () => setDeploy(false), [id, setDeploy]);
  const kiln = hasKiln(s, st.data);
  const kilnInstalledNow = !!(st.data?.installed || st.data?.kiln);
  const remote = s?.runner === "remote" || s?.kind === "remote";
  const players = st.data?.players ?? query.data?.players ?? null;
  const max = query.data?.maxPlayers ?? null;
  const who = players == null ? "" : ` ${whoLine(players, max, max != null ? max - players : 0)}`;
  let say: ReactNode;
  if (phase === "starting") say = <>{t("servers.head.starting")}{live && <Connection />}</>;
  else if (phase === "stopping") say = t("servers.head.stopping");
  else if (phase === "updating") say = t("servers.activity.updating");
  else if (!st.data) say = t("servers.head.reading");
  else if (state === "running") say = <>{st.data.uptimeS != null ? t("servers.head.runningFor", { time: dur(st.data.uptimeS) }) : t("servers.head.running")}{live ? <LiveWho max={max} fallback={who} /> : who}{live && <Connection />}</>;
  else if (state === "starting") say = <>{t("servers.head.starting")}{live && <Connection />}</>;
  else if (state === "stopping") say = t("servers.head.stopping");
  else {
    const runner = s ? runnerLabel(s.runner ?? "kilnctl") : null;
    const kilnV = st.data.kiln;
    say = runner && kilnV ? t("servers.head.stoppedWithKiln", { runner, kiln: kilnV }) : runner ? t("servers.head.stoppedWith", { runner })
      : kilnV ? t("servers.head.stoppedKiln", { kiln: kilnV }) : t("servers.head.stopped");
  }
  // the state word beside the name: the running step's first, then the backend's
  const shown = phase ?? state;
  const stateWord = !st.data && !phase ? null
    : shown === "starting" ? { word: t("servers.list.starting"), tone: "ember" }
    : shown === "stopping" ? { word: t("servers.list.stopping"), tone: "warn" }
    : shown === "updating" ? { word: t("servers.frame.updating"), tone: "warn" }
    : shown === "running" ? { word: t("servers.list.running"), tone: "ok" }
    : { word: t("servers.list.stopped"), tone: "idle" };
  // the buttons follow the running step first: Stop stays usable while a start waits for the world
  // a process here, or a remote server that answers (its status has no pid: kilnctl runs it over there)
  const up = !!st.data?.pid || (remote && state === "running");
  const running = phase ? phase === "starting" || (phase === "stopping" && busy !== "stop" && up) : state === "running" || state === "starting";
  const canStop = busy === "start" || (!busy && up);
  const items: MenuItem[] = [
    // SteamCMD updates a folder on this PC: not a Steam library install (Steam does), not a remote server
    ...(s?.runner !== "steam" && !remote ? [{ label: t("servers.head.updateGame"), disabled: !!busy || state === "starting" || state === "stopping", onSelect: updateGame }] : []),
    ...(!remote ? [{ label: kiln ? t("servers.head.updateKilnMenu") : t("servers.head.installKilnMenu"), disabled: !!busy || state === "starting" || state === "stopping", onSelect: () => setDeploy(true) }] : []),
    ...(live ? transportItems(id) : []),
    { label: t("servers.head.removeMenu"), danger: true, disabled: !!st.data?.pid || !!busy, onSelect: () => setRemoving(true) },
  ];
  return (
    <header className="k-page-head k-server-head">
      <div className="k-server-name" style={{ minWidth: 0 }}>
        <h1 className="k-page-title k-server-title">
          {s ? (
            <button type="button" className="k-name-btn" aria-haspopup="listbox" aria-expanded={pick} onClick={() => setPick((p) => !p)} aria-label={t("servers.head.changeServer", { name: s.name })}>
              {s.name}<svg viewBox="0 0 24 24" aria-hidden><path d="M5 9l7 7 7-7" /></svg>
            </button>
          ) : <Skeleton lines={1} width={260} height={46} />}
        </h1>
        {stateWord && <span className={`k-word k-tone-${stateWord.tone} k-server-word`}>{stateWord.word}</span>}
        {pick && <ServerPicker current={id} servers={servers.data ?? []} onClose={() => setPick(false)} onPick={(sid) => { setPick(false); nav(`/servers/${encodeURIComponent(sid)}/overview`); }} />}
        <p className="k-page-sub" style={{ marginTop: 1 }}>{say}</p>
      </div>
      <div className="k-page-actions" style={{ paddingTop: 8 }}>
        {running ? (
          <>
            <Button disabled={!!busy || state !== "running"} loading={busy === "restart"} onClick={restart}>{t("servers.head.restart")}</Button>
            <Button variant="danger" disabled={!canStop} onClick={stop}>{t("servers.head.stop")}</Button>
          </>
        ) : busy === "stop" ? (
          <Button variant="danger" disabled loading>{t("servers.activity.stoppingButton")}</Button>
        ) : (
          <Button variant="primary" disabled={!!busy || !st.data} loading={!!busy} onClick={start}>{busy && busy !== "start" ? t("servers.activity.busyButton") : t("servers.head.start")}</Button>
        )}
        <Menu label={t("servers.head.more")} icon={<MoreHorizontal />} items={items} />
      </div>
      <DeployModal open={deploy} update={kilnInstalledNow} running={pid} current={st.data?.installed ?? st.data?.kiln ?? null} onClose={() => setDeploy(false)}
        onDeploy={(v, startAfter) => { setDeploy(false); deployKiln(v, startAfter); }} />
      <ConfirmDanger open={removing} onClose={() => setRemoving(false)} title={t("servers.head.removeTitle")}
        message={t("servers.head.removeText")}
        word={id} action={t("common.remove")} onConfirm={async () => {
          try {
            await api.serverRemove(id, false);
            toast.ok(t("servers.head.removed", { name: s?.name ?? id }), t("servers.head.filesStay"));
            qc.invalidateQueries({ queryKey: sk.list });
            nav("/servers");
          } catch (e) { toast.error(e, t("servers.head.notRemoved")); throw e; }
        }} />
    </header>
  );
}

/** " 2 online, 2 slots free." from the admin session's state when it has one (inside an AdminProvider). */
function LiveWho({ max, fallback }: { max: number | null; fallback: string }) {
  useT();
  const st = useAdminState();
  const n = st.state?.players?.length;
  if (n == null) return <>{fallback}</>;
  return <>{` ${whoLine(n, max, max != null ? Math.max(0, max - n) : 0)}`}</>;
}

/** " Connected. KAP/1 TCP, 151 ops." (inside an AdminProvider). */
function Connection() {
  const t = useT();
  const st = useAdminState();
  const T: Record<string, string> = { tcp: "KAP/1 TCP", file: t("servers.head.fileBridge"), ssh: t("servers.overview.sshTunnel"), auto: t("servers.head.auto") };
  if (st.ready && st.session) return <> {rich("servers.head.connected", { connected: <span className="k-tone-ok">{t("servers.head.connectedWord")}</span>, transport: T[st.session.transport] ?? st.session.transport, ops: st.session.ops.length })}</>;
  if (st.connecting) return <> {t("servers.head.connecting")}</>;
  if (st.status === "error") return <> <span className="k-tone-warn">{t("servers.head.notAnswering")}</span></>;
  return null;
}

function transportItems(id: string): MenuItem[] {
  const T: [AdminTransport, string][] = [["auto", t("servers.head.transportAuto")], ["tcp", t("servers.head.transportTcp")], ["file", t("servers.overview.fileBridge")], ["ssh", t("servers.overview.sshTunnel")]];
  return [
    { label: t("servers.head.reconnect"), onSelect: () => reconnectAdmin(id) },
    ...T.map(([transport, l]) => ({ label: t("servers.head.connectOver", { transport: l }), onSelect: () => reconnectAdmin(id, transport) })),
  ];
}

function ServerPicker({ current, servers, onClose, onPick }: { current: string; servers: { id: string; name: string }[]; onClose: () => void; onPick: (id: string) => void }) {
  const t = useT();
  const ref = useRef<HTMLDivElement>(null);
  const nav = useNavigate();
  useEffect(() => {
    const close = (e: MouseEvent) => { if (!ref.current?.contains(e.target as Node) && !(e.target as HTMLElement).closest?.(".k-name-btn")) onClose(); };
    const esc = (e: KeyboardEvent) => { if (e.key === "Escape") onClose(); };
    document.addEventListener("mousedown", close);
    document.addEventListener("keydown", esc);
    // the current server first, as a list box does
    (ref.current?.querySelector<HTMLElement>('.k-menu-item[aria-selected="true"]') ?? ref.current?.querySelector<HTMLElement>(".k-menu-item"))?.focus();
    return () => { document.removeEventListener("mousedown", close); document.removeEventListener("keydown", esc); };
  }, [onClose]);
  // the arrows (and Home, End) move between the servers and "All servers"
  const keys = (e: ReactKeyboardEvent) => {
    const items = [...(ref.current?.querySelectorAll<HTMLElement>(".k-menu-item") ?? [])];
    const at = items.indexOf(document.activeElement as HTMLElement);
    const to = e.key === "ArrowDown" ? (at + 1) % items.length : e.key === "ArrowUp" ? (at - 1 + items.length) % items.length
      : e.key === "Home" ? 0 : e.key === "End" ? items.length - 1 : -1;
    if (to < 0 || !items.length) return;
    e.preventDefault();
    items[to].focus();
  };
  return (
    <div ref={ref} className="k-popover" role="listbox" aria-label={t("servers.list.title")} style={{ left: 0, top: 60, width: 320 }} onKeyDown={keys}>
      {servers.map((s) => (
        <button key={s.id} type="button" role="option" aria-selected={s.id === current} className="k-menu-item" onClick={() => onPick(s.id)}>
          <span style={{ fontWeight: s.id === current ? 700 : 400 }}>{s.name}</span><span className="k-mono">{s.id}</span>
        </button>
      ))}
      <div className="k-menu-sep" />
      <button type="button" className="k-menu-item" style={{ color: "var(--k-ember-300)" }} onClick={() => { onClose(); nav("/servers"); }}>{t("servers.head.allServers")}</button>
    </div>
  );
}

function DeployModal({ open, update, running, current, onClose, onDeploy }: {
  open: boolean; update: boolean; running: boolean; current: string | null; onClose: () => void; onDeploy: (version: string, startAfter: boolean) => void;
}) {
  const t = useT();
  const bundles = useQuery({ queryKey: ["runtime_list"], queryFn: api.runtimeList, enabled: open });
  const [v, setV] = useState("");
  const [startAfter, setStartAfter] = useState(false);
  // a fresh choice each time the dialog opens
  useEffect(() => { if (open) { setStartAfter(false); setV(""); } }, [open]);
  const chosen = v || bundles.data?.[0]?.version || "";
  // a running server is stopped first and always started again afterwards; a stopped one starts only when asked
  const again = running || startAfter;
  const steps = [running && t("servers.activity.stepStop"), t("servers.activity.stepDeploy", { version: chosen || "…" }), again && (running ? t("servers.activity.stepStartAgain") : t("servers.activity.stepStart"))]
    .filter(Boolean) as string[];
  return (
    <Modal open={open} onClose={onClose} title={update ? t("servers.head.updateKiln") : t("servers.head.installKiln")} width={520}
      footer={<><Button variant="ghost" onClick={onClose}>{t("common.cancel")}</Button><Button variant="primary" disabled={!chosen} onClick={() => onDeploy(chosen, again)}>{update ? t("common.update") : t("common.install")}</Button></>}>
      <div className="k-stack">
        <p className="k-muted" style={{ margin: 0 }}>{current ? t("servers.activity.installedNow", { version: current }) : t("servers.activity.notInstalled")}</p>
        <Field label={t("servers.head.bundle")} hint={t("servers.activity.bundleHint")} htmlFor="deploy-v">
          <Select id="deploy-v" value={chosen} onChange={setV} options={(bundles.data ?? []).map((b) => ({ value: b.version, label: t("servers.head.bundleOption", { version: b.version, kiln: b.kiln }) }))} />
        </Field>
        {running
          ? <p className="k-muted" style={{ margin: 0 }}>{t("servers.activity.runningNote")}</p>
          : <Toggle checked={again} onChange={setStartAfter} label={t("servers.activity.startAfter")} />}
        <ol className="k-sact-plan" aria-label={t("servers.activity.planLabel")}>
          {steps.map((s, i) => <li key={i}>{s}</li>)}
        </ol>
      </div>
    </Modal>
  );
}
