import { useQuery } from "@tanstack/react-query";
import { useState, type ReactNode } from "react";
import { useGame, useGameStatus, usePoll } from "../api/hooks";
import { useLanguage, useT } from "../i18n";
import { Banner, Button, Card, EmptyState, Page, PageHeader, Skeleton, StatusPill, WellInput, errorText } from "../ui";
import { readRegistryReport } from "./api";
import { parseRegistryReport, reportFreshness, type RegistryField } from "./report";
import "./registry.css";

export default function RegistryConflictsPage() {
  const t = useT();
  const { game, loading, error } = useGame();
  const title = t("registry.title");
  if (loading) return <Page><PageHeader title={title} /><Skeleton /></Page>;
  if (error) return <Page><PageHeader title={title} /><EmptyState title={t("registry.gameUnavailable")} hint={errorText(error)} /></Page>;
  if (!game) return <Page><PageHeader title={title} /><EmptyState title={t("registry.noGame")} hint={t("registry.noGameHint")} /></Page>;
  return <RegistryFor key={game.id} gameId={game.id} />;
}

function RegistryFor({ gameId }: { gameId: string }) {
  const t = useT();
  const language = useLanguage();
  const status = useGameStatus(gameId);
  // the parser's labels are in the current language: a language change parses again
  const query = useQuery({ queryKey: ["registry_report_read", gameId, language], retry: false, refetchInterval: usePoll(10_000), queryFn: async () => {
    const text = await readRegistryReport(gameId);
    return text === null ? null : parseRegistryReport(text);
  } });
  const [filter, setFilter] = useState("");
  const r = query.data;
  const stale = r ? reportFreshness(r.written, status.data) : null;
  const fields = r?.fields.filter(f => `${f.registry} ${f.id} ${f.field} ${f.winner} ${f.writers.map(w => w.owner).join(" ")}`.toLowerCase().includes(filter.toLowerCase())) ?? [];
  return <Page className="registry-conflicts">
    <PageHeader title={t("registry.title")} sub={t("registry.sub")}
      actions={<Button loading={query.isFetching} onClick={() => { void query.refetch(); void status.refetch(); }}>{t("registry.refresh")}</Button>} />
    {query.isLoading ? <Skeleton /> : query.error ? <EmptyState title={t("registry.unavailable")} hint={errorText(query.error)} />
      : !r ? <EmptyState title={t("registry.missing")} hint={t("registry.missingHint")} />
      : <>
        {stale && <Banner tone="warn" title={t("registry.stale")}>{t("registry.staleBody", { reason: stale })}</Banner>}
        <Card title={t("registry.snapshot")} sub={t("registry.snapshotSub", { time: new Date(r.written).toLocaleString(language), generation: r.generation, phase: r.phase })}>
          <p>{t("registry.baseOrigin", { origin: r.baseOrigin })}</p><p>{t("registry.archiveWriter", { writer: r.baseWriter })}</p>
          {!r.verified && <Banner tone="warn" title={t("registry.unverified")}>{t("registry.unverifiedBody")}</Banner>}
          <p className="registry-hint">{t("registry.certaintyHint")}</p>
        </Card>
        {(r.omitted > 0 || r.writersOmitted > 0) && <Banner tone="warn" title={t("registry.partial")}>
          {r.omitted > 0 && `${t("registry.partialRows", { count: r.omitted })} `}
          {r.writersOmitted > 0 && `${t("registry.partialWriters", { count: r.writersOmitted })} `}
          {t("registry.partialBody")}</Banner>}
        {r.empty ? <EmptyState title={t("registry.empty")} hint={t("registry.emptyHint")} /> : <>
          <Card title={t("registry.refused")} sub={t("registry.refusedSub")}>
            <Rows rows={r.refused} empty={t("registry.noRefused")} render={(x, i) => <article key={i} className="registry-line">
              <strong>{x.owner}</strong> · {x.stage}{x.field && <div className="k-mono">{x.field}</div>}<p>{x.reason}</p>
            </article>} />
          </Card>
          <Card title={t("registry.conflicts")} sub={t("registry.conflictsSub")}>
            <Rows rows={r.conflicts} empty={t("registry.noConflicts")} render={(x, i) => <article key={i} className="registry-line">
              <div className="registry-line-head"><strong>{x.field}</strong><StatusPill tone={x.certain ? "ok" : "warn"}>{x.certainty}</StatusPill></div>
              <p>{t("registry.conflictLine", { kind: x.kind, winner: x.winner, loser: x.loser })}</p><p>{x.text}</p>
            </article>} />
          </Card>
          <Card title={t("registry.fields")} sub={t("registry.fieldsSub")}>
            <WellInput label={t("registry.filter")} value={filter} onChange={e => setFilter(e.target.value.slice(0, 256))} onClear={() => setFilter("")} />
            <Rows key={filter} rows={fields} empty={filter ? t("registry.noMatch") : t("registry.noFields")} render={(x, i) => <FieldChain key={i} field={x} />} />
          </Card>
          {r.entries.length > 0 && <Card title={t("registry.entries")}><Rows rows={r.entries} empty={t("registry.noEntries")} render={(x, i) => <p key={i}>{x.kind} · {x.registry} · {x.id}</p>} /></Card>}
        </>}
      </>}
  </Page>;
}

function Rows<T>({ rows, render, empty }: { rows: T[]; render: (row: T, index: number) => ReactNode; empty: string }) {
  const t = useT();
  const [page, setPage] = useState(0);
  const pages = Math.max(1, Math.ceil(rows.length / 25));
  const current = Math.min(page, pages - 1);
  if (!rows.length) return <EmptyState title={empty} />;
  return <>
    {rows.slice(current * 25, (current + 1) * 25).map((row, i) => render(row, current * 25 + i))}
    {pages > 1 && <div className="registry-paging"><Button size="s" disabled={current === 0} onClick={() => setPage(current - 1)}>{t("registry.previous")}</Button>
      <span>{t("registry.page", { page: current + 1, pages, count: rows.length })}</span>
      <Button size="s" disabled={current + 1 >= pages} onClick={() => setPage(current + 1)}>{t("registry.next")}</Button></div>}
  </>;
}

function FieldChain({ field: f }: { field: RegistryField }) {
  const t = useT();
  const [open, setOpen] = useState(false);
  return <details className="registry-field" onToggle={e => setOpen(e.currentTarget.open)}>
    <summary><strong>{f.registry} · {f.id} · {f.field}</strong><span>{t("registry.summary", { winner: f.winner, result: f.result })}{f.writersOmitted > 0 && ` · ${t("registry.summaryOmitted", { count: f.writersOmitted })}`}</span></summary>
    {open && <div className="registry-chain">
      <p>{t("registry.applies", { applies: f.applies, origin: f.baseOrigin })}</p><p className="k-mono">{t("registry.baseValue", { value: f.base })}</p>
      {f.writersOmitted > 0 && <p className="k-tone-warn">{t("registry.writersOmitted", { count: f.writersOmitted })}</p>}
      <ol>{f.writers.map((w, i) => <li key={i}>
        <div className="registry-line-head"><strong>{w.owner}</strong><StatusPill tone={w.certain ? "ok" : "warn"}>{w.certainty}</StatusPill></div>
        <p>{w.kind}{w.stage && ` · ${w.stage}`}</p><p className="k-mono">{t("registry.operation", { operation: w.operation })}</p><p className="k-mono">{t("registry.after", { value: w.value })}</p>
      </li>)}</ol>
      {!f.writers.length && <p>{t("registry.noWriters")}</p>}
      {f.formulaInputs && <p className="k-mono">{t("registry.formula", { inputs: f.formulaInputs })}</p>}
      <p><strong>{t("registry.winner", { winner: f.winner })}</strong> · {t("registry.result")} <span className="k-mono">{f.result}</span></p>
    </div>}
  </details>;
}
