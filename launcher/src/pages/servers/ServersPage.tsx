// /servers: every dedicated server the launcher knows, one row each (servers revamp, mockup A "Hearth dashboard",
// 2026-11-22). The header says it in one sentence ("3 servers. Ember Isles is running with 3 players; Hollow Lab has a
// Kiln update."); a banner offers the Kiln update when a server's Kiln is older than the newest runtime bundle
// (kilnUpdate.ts) and runs the Update Kiln flow (stop, install, start again when it ran); the table shows each
// server's state (or its running flow, live, as in mockup B), players, health, Kiln and where it runs, with Start/Stop,
// Console or Update Kiln and a ··· menu (Restart, the console, Back up now, Update Kiln, Remove); "Recent" lists the
// last actions (activityLog.ts). New server opens the wizard (NewServerWizard.tsx); Import adopts a server folder.
import { useQueries, useQuery, useQueryClient } from "@tanstack/react-query";
import { useEffect, useMemo, useState } from "react";
import { useNavigate } from "react-router-dom";
import * as api from "../../api/commands";
import type { RuntimeBundle } from "../../api/commands";
import { qk, usePoll } from "../../api/hooks";
import {
  Banner, Button, ConfirmDanger, EmptyState, Field, Modal, NameId, Page, PageHeader, PathPicker, Skeleton, Table, TextInput, errorText, toast, type Column,
} from "../../ui";
import { useT } from "../../i18n";
import { TICKS } from "../ticks";
import { sk, useServerEvents, useServers, type ServerInstanceX } from "./serverApi";
import { flowRunning, retryFlow, runFlow, useServerFlows } from "./serverActions";
import { backupFlow, kilnFlow, restartFlow, startFlow, stopServer } from "./serverFlows";
import { kilnUpdateFor, newestServerBundle } from "./kilnUpdate";
import { stoppedSince, useActivity } from "./activityLog";
import { RecentActivity } from "./RecentActivity";
import { NewServerWizard } from "./NewServerWizard";
import { ActionsCell, HealthCell, KilnCell, OnlineCell, RunsOnCell, StateCell, type RowActions, type ServerRow } from "./serverListCells";
import { listSentence } from "./listSay";
import "./servers-list.css";

/** Windows (WebView2): path examples in Windows form. */
const WIN = /Windows/i.test(navigator.userAgent);
const ROW = 56;

function useNow(on: boolean): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    setNow(Date.now());
    if (!on) return;
    const h = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(h);
  }, [on]);
  return now;
}

/** Steps per second of the tick stats' last minutes (the file's tail: one line a minute). */
function rates(text: string | undefined): number[] {
  return (text ?? "").split("\n").flatMap((l) => {
    try { const r = l.trim() ? (JSON.parse(l) as { rate?: unknown }).rate : undefined; return typeof r === "number" && Number.isFinite(r) ? [r] : []; } catch { return []; }
  }).slice(-16);
}

/** The Kiln update: what is out, which server runs an older one, one button; the update while it runs; its failure. */
function UpdateBanner({ rows, newest, onUpdate, onRetry }: { rows: ServerRow[]; newest: RuntimeBundle | null; onUpdate: (rows: ServerRow[]) => void; onRetry: (r: ServerRow) => void }) {
  const t = useT();
  const running = rows.find((r) => r.flow?.kind === "deploy" && r.flow.state === "running");
  if (running) {
    const f = running.flow!;
    const i = f.steps.findIndex((s) => s.state === "running");
    const step = f.steps[i];
    return (
      <Banner tone="ember" title={t("servers.list.banner.updatingTitle")}
        actions={<Button size="s" disabled loading>{t("servers.list.banner.updateOne", { name: running.srv.name })}</Button>}>
        {t("servers.list.banner.running", { name: running.srv.name, n: i + 1, count: f.steps.length, step: step?.label ?? "" })}
      </Banner>
    );
  }
  const failed = rows.find((r) => r.flow?.kind === "deploy" && r.flow.state === "failed");
  if (failed) {
    return (
      <Banner tone="err" title={t("servers.list.banner.failedTitle")}
        actions={<Button size="s" onClick={() => onRetry(failed)}>{t("servers.activity.tryAgain")}</Button>}>
        {t("servers.list.banner.failed", { name: failed.srv.name, reason: failed.flow?.error?.message ?? t("servers.activity.noReason") })}
      </Banner>
    );
  }
  const need = rows.filter((r) => r.update && !r.remote);
  if (!need.length || !newest) return null;
  const one = need[0];
  const moving = need.some((r) => r.flow?.state === "running" || r.st?.state === "starting" || r.st?.state === "stopping");
  return (
    <Banner tone="warn" title={t("servers.list.banner.title")}
      actions={<Button size="s" disabled={moving} onClick={() => onUpdate(need)}>
        {need.length === 1 ? t("servers.list.banner.updateOne", { name: one.srv.name }) : t("servers.list.banner.updateAll", { count: need.length })}
      </Button>}>
      {need.length > 1 ? t("servers.list.banner.several", { version: newest.version, names: need.map((r) => r.srv.name).join(", ") })
        : one.st?.pid ? t("servers.list.banner.oneRunning", { version: newest.version, name: one.srv.name, current: one.kiln ?? "" })
        : t("servers.list.banner.single", { version: newest.version, name: one.srv.name, current: one.kiln ?? "" })}
    </Banner>
  );
}

function ImportServer({ open, onClose }: { open: boolean; onClose: () => void }) {
  const t = useT();
  const qc = useQueryClient();
  const [dir, setDir] = useState("");
  const [name, setName] = useState("");
  const [busy, setBusy] = useState(false);
  const go = async () => {
    setBusy(true);
    try {
      const s = await api.serverImport(dir, name || "server");
      toast.ok(t("servers.list.imported", { name: s.name }));
      qc.invalidateQueries({ queryKey: sk.list });
      onClose();
    } catch (e) {
      toast.error(e, t("servers.list.importFailed"));
    } finally {
      setBusy(false);
    }
  };
  return (
    <Modal open={open} onClose={onClose} title={t("servers.list.importTitle")}
      footer={<><Button variant="ghost" onClick={onClose}>{t("common.cancel")}</Button><Button variant="primary" onClick={go} disabled={!dir} loading={busy}>{t("servers.list.import")}</Button></>}>
      <div className="k-stack">
        <Field label={t("servers.list.serverFolder")} hint={WIN ? t("servers.list.serverFolderHintWin") : t("servers.list.serverFolderHint")}>
          <PathPicker value={dir} onChange={setDir} directory />
        </Field>
        <Field label={t("servers.list.name")}><TextInput value={name} onChange={(e) => setName(e.target.value)} /></Field>
      </div>
    </Modal>
  );
}

export default function ServersPage() {
  const t = useT();
  useServerEvents();
  const nav = useNavigate();
  const qc = useQueryClient();
  const q = useServers();
  const [creating, setCreating] = useState(false);
  const [importing, setImporting] = useState(false);
  const [removing, setRemoving] = useState<ServerInstanceX | null>(null);
  const list = useMemo(() => q.data ?? [], [q.data]);
  const flows = useServerFlows((s) => s.flows);
  const entries = useActivity((s) => s.entries);
  const bundles = useQuery({ queryKey: qk.runtimeList, queryFn: api.runtimeList });
  const newest = newestServerBundle(bundles.data);
  const poll15 = usePoll(15000), poll30 = usePoll(30000);
  // the same status query as each server's page (useServerStatus): every second while it moves, else every 5 s
  const statuses = useQueries({
    queries: list.map((s) => ({
      queryKey: sk.status(s.id), queryFn: () => api.serverStatus(s.id),
      refetchInterval: (qq: { state: { data?: api.ServerStatus } }) => {
        const st = qq.state.data?.state;
        return st === "starting" || st === "stopping" || flowRunning(s.id) ? 1000 : 5000;
      },
    })),
  });
  const st = (i: number) => statuses[i]?.data;
  const isRemote = (s: ServerInstanceX) => s.kind === "remote" || s.runner === "remote";
  // the slots of a running server (the server's own answer to a query), as the server head reads them
  const queries = useQueries({
    queries: list.map((s, i) => ({ queryKey: sk.query(s.id), queryFn: () => api.serverQuery(s.id), enabled: st(i)?.state === "running", refetchInterval: poll15, retry: false })),
  });
  // health: the tick stats' last minutes of a running server on this PC (an 8 KB tail every 30 s, nothing when stopped)
  const ticks = useQueries({
    queries: list.map((s, i) => ({
      queryKey: ["server-tick-tail", s.id], queryFn: () => api.logsRead({ server: s.id }, TICKS, null, 8192),
      enabled: st(i)?.state === "running" && !isRemote(s), refetchInterval: poll30, retry: false,
    })),
  });
  const rows: ServerRow[] = list.map((s, i) => {
    const status = st(i);
    const remote = isRemote(s);
    const kiln = remote ? status?.kiln ?? null : status?.installed ?? status?.kiln ?? null;
    return {
      srv: s, st: status, flow: flows[s.id], remote, kiln,
      update: kilnUpdateFor(status?.installed, bundles.data, remote),
      maxPlayers: queries[i]?.data?.maxPlayers ?? null,
      rates: status?.state === "running" && !remote ? rates(ticks[i]?.data?.text) : [],
    };
  });
  const anyRunning = rows.some((r) => r.flow?.state === "running");
  const now = useNow(anyRunning);
  const enc = encodeURIComponent;
  const act: RowActions = {
    start: (r) => void runFlow(qc, r.srv.id, startFlow(r.srv.id, r.srv.name)),
    stop: (r) => void stopServer(qc, r.srv.id, r.srv.name),
    restart: (r) => void runFlow(qc, r.srv.id, restartFlow(r.srv.id, r.srv.name)),
    backup: (r) => void runFlow(qc, r.srv.id, backupFlow(r.srv.id, r.srv.name)),
    kiln: (r) => {
      const version = r.update ?? newest?.version;
      if (!version) return;
      void runFlow(qc, r.srv.id, kilnFlow({ id: r.srv.id, name: r.srv.name, version, running: !!r.st?.pid, from: r.kiln }));
    },
    console: (r) => nav(`/servers/${enc(r.srv.id)}/overview`),
    remove: (r) => setRemoving(r.srv),
  };
  // Update all: one server after the other (each its own flow, shown in its row)
  const updateAll = async (need: ServerRow[]) => {
    for (const r of need) {
      const version = r.update ?? newest?.version;
      if (!version) continue;
      await runFlow(qc, r.srv.id, kilnFlow({ id: r.srv.id, name: r.srv.name, version, running: !!r.st?.pid, from: r.kiln }));
    }
  };
  const names = Object.fromEntries(list.map((s) => [s.id, s.name]));
  const cols: Column<ServerRow>[] = [
    { key: "name", header: t("servers.list.server"), width: "minmax(170px, 1.35fr)", sort: (a, b) => a.srv.name.localeCompare(b.srv.name), render: (r) => <NameId name={r.srv.name} id={r.srv.id} /> },
    { key: "state", header: t("servers.list.state"), width: "minmax(170px, 1.15fr)", render: (r) => <StateCell row={r} now={now} since={stoppedSince(entries, r.srv.id)} /> },
    { key: "players", header: t("servers.list.online"), width: "64px", align: "right", render: (r) => <OnlineCell row={r} /> },
    { key: "health", header: t("servers.list.health"), width: "112px", render: (r) => <HealthCell row={r} /> },
    { key: "kiln", header: t("servers.list.kiln"), width: "minmax(0, 182px)", render: (r) => <KilnCell row={r} /> },
    { key: "runner", header: t("servers.list.runsOn"), width: "minmax(0, 150px)", render: (r) => <RunsOnCell srv={r.srv} /> },
    { key: "act", header: t("servers.list.actions"), width: "232px", align: "right", render: (r) => <ActionsCell row={r} act={act} target={newest?.version ?? null} /> },
  ];
  const say = q.isLoading ? t("servers.list.reading") : q.error ? errorText(q.error) : !list.length ? t("servers.list.noneYet") : listSentence(rows, t);
  return (
    <Page fill className="k-servers">
      <PageHeader title={t("servers.list.title")} sub={say}
        actions={<><Button onClick={() => setImporting(true)}>{t("servers.list.import")}</Button><Button variant="primary" onClick={() => setCreating(true)}>{t("servers.list.newServer")}</Button></>} />
      {!!list.length && <UpdateBanner rows={rows} newest={newest} onUpdate={(need) => void updateAll(need)} onRetry={(r) => void retryFlow(qc, r.srv.id)} />}
      {q.isLoading ? <Skeleton lines={3} /> : q.error ? null : !list.length ? (
        <EmptyState page title={t("servers.list.noServers")} hint={t("servers.list.noServersHint")} action={<Button onClick={() => setCreating(true)}>{t("servers.list.newServer")}</Button>} />
      ) : (
        <Table aria-label={t("servers.list.title")} columns={cols} rows={rows} rowKey={(r) => r.srv.id} rowHeight={ROW} maxHeight={ROW * 6}
          onRowClick={(r) => nav(`/servers/${enc(r.srv.id)}/overview`)} />
      )}
      {!q.isLoading && !q.error && (!!list.length || !!entries.length) && <RecentActivity entries={entries} names={names} now={now} />}
      <NewServerWizard open={creating} onClose={() => setCreating(false)} servers={list} bundles={bundles.data} />
      <ImportServer open={importing} onClose={() => setImporting(false)} />
      <ConfirmDanger open={!!removing} onClose={() => setRemoving(null)} title={t("servers.head.removeTitle")} message={t("servers.head.removeText")}
        word={removing?.id ?? ""} action={t("common.remove")} onConfirm={async () => {
          const s = removing;
          if (!s) return;
          try {
            await api.serverRemove(s.id, false);
            toast.ok(t("servers.head.removed", { name: s.name }), t("servers.head.filesStay"));
            useActivity.getState().add({ at: Date.now(), serverId: s.id, name: s.name, kind: "remove", state: "done", ms: 0, steps: [] });
            useServerFlows.getState().dismiss(s.id);
            qc.invalidateQueries({ queryKey: sk.list });
          } catch (e) { toast.error(e, t("servers.head.notRemoved")); throw e; }
        }} />
    </Page>
  );
}
