import { useQuery } from "@tanstack/react-query";
import { useState, type ReactNode } from "react";
import { useGame, useGameStatus, usePoll } from "../api/hooks";
import { Banner, Button, Card, EmptyState, Page, PageHeader, Skeleton, StatusPill, WellInput, errorText } from "../ui";
import { readRegistryReport } from "./api";
import { parseRegistryReport, reportFreshness, type RegistryField } from "./report";
import "./registry.css";

export default function RegistryConflictsPage() {
  const { game, loading, error } = useGame();
  if (loading) return <Page><PageHeader title="Registry conflicts" /><Skeleton /></Page>;
  if (error) return <Page><PageHeader title="Registry conflicts" /><EmptyState title="Game selection unavailable" hint={errorText(error)} /></Page>;
  if (!game) return <Page><PageHeader title="Registry conflicts" /><EmptyState title="No game selected" hint="Choose an installed game in Settings to read its registry snapshot." /></Page>;
  return <RegistryFor key={game.id} gameId={game.id} />;
}

function RegistryFor({ gameId }: { gameId: string }) {
  const status = useGameStatus(gameId);
  const query = useQuery({ queryKey: ["registry_report_read", gameId], retry: false, refetchInterval: usePoll(10_000), queryFn: async () => {
    const text = await readRegistryReport(gameId);
    return text === null ? null : parseRegistryReport(text);
  } });
  const [filter, setFilter] = useState("");
  const r = query.data;
  const stale = r ? reportFreshness(r.written, status.data) : null;
  const fields = r?.fields.filter(f => `${f.registry} ${f.id} ${f.field} ${f.winner} ${f.writers.map(w => w.owner).join(" ")}`.toLowerCase().includes(filter.toLowerCase())) ?? [];
  return <Page className="registry-conflicts">
    <PageHeader title="Registry conflicts" sub="The saved registry snapshot for this game, not live telemetry or a prediction of the next start."
      actions={<Button loading={query.isFetching} onClick={() => { void query.refetch(); void status.refetch(); }}>Refresh report</Button>} />
    {query.isLoading ? <Skeleton /> : query.error ? <EmptyState title="Registry report unavailable" hint={errorText(query.error)} />
      : !r ? <EmptyState title="No registry report yet" hint="Kiln has not written kiln/state/registry.json for this game. There is no registry snapshot to inspect." />
      : <>
        {stale && <Banner tone="warn" title="Stale snapshot">{stale} Values below belong to the saved snapshot, not necessarily the current world or mod list.</Banner>}
        <Card title="Snapshot" sub={`${new Date(r.written).toLocaleString()} · generation ${r.generation} · ${r.phase}`}>
          <p>Base origin: {r.baseOrigin}</p><p>Archive writer: {r.baseWriter}</p>
          {!r.verified && <Banner tone="warn" title="Mapping unverified">Field edits may be refused as Unproven. Read each refusal for its reason.</Banner>}
          <p className="registry-hint">Certain = Kiln observed the writer. Named from journal = archive-writer attribution. Best-effort drift is not proof of who wrote it or that no other writer exists.</p>
        </Card>
        {r.omitted > 0 && <Banner tone="warn" title="Partial report">{r.omitted} rows were omitted by the native report or this view's safety limits. Text and list values are also bounded; this is not the full history.</Banner>}
        {r.empty ? <EmptyState title="No registry edits in this snapshot" hint="The report is present and valid, but contains no fields, conflicts, refusals or entry changes." /> : <>
          <Card title="Refused edits" sub="These edits were not applied. RestartRequired needs a game restart; Unproven needs a proven mapping in Kiln.">
            <Rows rows={r.refused} empty="No refused edits in the displayed report." render={(x, i) => <article key={i} className="registry-line">
              <strong>{x.owner}</strong> · {x.stage}{x.field && <div className="k-mono">{x.field}</div>}<p>{x.reason}</p>
            </article>} />
          </Card>
          <Card title="Conflicts and drift" sub="Winners and other owners are as recorded at snapshot time.">
            <Rows rows={r.conflicts} empty="No conflict lines in the displayed report." render={(x, i) => <article key={i} className="registry-line">
              <div className="registry-line-head"><strong>{x.field}</strong><StatusPill tone={x.certainty === "Certain" ? "ok" : "warn"}>{x.certainty}</StatusPill></div>
              <p>{x.kind} · Winner: {x.winner} · Other owner: {x.loser}</p><p>{x.text}</p>
            </article>} />
          </Card>
          <Card title="Fields and writer chains" sub="Earlier writers first; expand a field for its base, operations and result.">
            <WellInput label="Filter fields or owners" value={filter} onChange={e => setFilter(e.target.value.slice(0, 256))} onClear={() => setFilter("")} />
            <Rows key={filter} rows={fields} empty={filter ? "No displayed fields match this filter." : "No field rows in the displayed report."} render={(x, i) => <FieldChain key={i} field={x} />} />
          </Card>
          {r.entries.length > 0 && <Card title="Entry changes"><Rows rows={r.entries} empty="No entry changes." render={(x, i) => <p key={i}>{x.kind} · {x.registry} · {x.id}</p>} /></Card>}
        </>}
      </>}
  </Page>;
}

function Rows<T>({ rows, render, empty }: { rows: T[]; render: (row: T, index: number) => ReactNode; empty: string }) {
  const [page, setPage] = useState(0);
  const pages = Math.max(1, Math.ceil(rows.length / 25));
  const current = Math.min(page, pages - 1);
  if (!rows.length) return <EmptyState title={empty} />;
  return <>
    {rows.slice(current * 25, (current + 1) * 25).map(render)}
    {pages > 1 && <div className="registry-paging"><Button size="s" disabled={current === 0} onClick={() => setPage(current - 1)}>Previous</Button>
      <span>Page {current + 1} of {pages} · {rows.length} displayed rows</span>
      <Button size="s" disabled={current + 1 >= pages} onClick={() => setPage(current + 1)}>Next</Button></div>}
  </>;
}

function FieldChain({ field: f }: { field: RegistryField }) {
  const [open, setOpen] = useState(false);
  return <details className="registry-field" onToggle={e => setOpen(e.currentTarget.open)}>
    <summary><strong>{f.registry} · {f.id} · {f.field}</strong><span>Winner: {f.winner} · Result: {f.result}</span></summary>
    {open && <div className="registry-chain">
      <p>Applies: {f.applies} · Base origin: {f.baseOrigin}</p><p className="k-mono">Base value: {f.base}</p>
      {f.writersOmitted > 0 && <p className="k-tone-warn">{f.writersOmitted} earlier writers omitted. Only the retained end of the chain is shown.</p>}
      <ol>{f.writers.map((w, i) => <li key={i}>
        <div className="registry-line-head"><strong>{w.owner}</strong><StatusPill tone={w.certainty === "Certain" ? "ok" : "warn"}>{w.certainty}</StatusPill></div>
        <p>{w.kind}{w.stage && ` · ${w.stage}`}</p><p className="k-mono">Operation: {w.operation}</p><p className="k-mono">After: {w.value}</p>
      </li>)}</ol>
      {!f.writers.length && <p>No writer rows; the base decides the result.</p>}
      {f.formulaInputs && <p className="k-mono">Formula inputs (evaluation order): {f.formulaInputs}</p>}
      <p><strong>Winner: {f.winner}</strong> · Result: <span className="k-mono">{f.result}</span></p>
    </div>}
  </details>;
}
