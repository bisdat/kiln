// The server's activity panel, under the server head: the flow that runs or just ended (serverActions.ts). While it
// runs: the title, the time it has taken, its steps (stop, install, start again) and the newest line the running step's
// task reported; "Show details" lists every line. When it ends: the result, or why it failed with Try again and the
// console. A finished flow closes by itself after a while unless its details are open.
import { useQueryClient } from "@tanstack/react-query";
import { Check, ChevronDown, ChevronUp, Minus, X } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { useNavigate } from "react-router-dom";
import { Button, Cones, IconButton, Spinner } from "../../ui";
import { useT } from "../../i18n";
import { elapsed, markShown, retryFlow, useServerFlow, useServerFlows, type FlowStep } from "./serverActions";

const CLOSE_AFTER_MS = 15_000;

function useNow(on: boolean): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (!on) return;
    setNow(Date.now());
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [on]);
  return now;
}

function StepMark({ step }: { step: FlowStep }) {
  if (step.state === "done") return <span className="k-sact-mark ok" aria-hidden><Check /></span>;
  if (step.state === "failed") return <span className="k-sact-mark err" aria-hidden><X /></span>;
  if (step.state === "running") return <span className="k-sact-mark run" aria-hidden><Spinner size={14} /></span>;
  return <span className="k-sact-mark idle" aria-hidden><Minus /></span>;
}

export function ServerActivity({ id }: { id: string }) {
  const t = useT();
  const qc = useQueryClient();
  const nav = useNavigate();
  const flow = useServerFlow(id);
  const dismiss = useServerFlows((s) => s.dismiss);
  const [open, setOpen] = useState(false);
  const running = flow?.state === "running";
  const now = useNow(running);
  const tick = Math.floor(now / 900);
  const linesRef = useRef<HTMLOListElement>(null);
  useEffect(() => markShown(id), [id]);
  // a finished flow closes by itself unless its details are open
  useEffect(() => {
    if (!flow || flow.state === "running" || open || flow.state === "failed") return;
    const left = CLOSE_AFTER_MS - (Date.now() - (flow.endedAt ?? Date.now()));
    const h = setTimeout(() => dismiss(id), Math.max(0, left));
    return () => clearTimeout(h);
  }, [flow, open, dismiss, id]);
  // the details follow the newest line
  useEffect(() => {
    const el = linesRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  });
  if (!flow) return null;
  const took = (flow.endedAt ?? now) - flow.startedAt;
  const cur = flow.steps.find((s) => s.state === "running") ?? [...flow.steps].reverse().find((s) => s.lines.length);
  const newest = cur?.lines[cur.lines.length - 1]?.text;
  const all = flow.steps.flatMap((s) => s.lines.map((l) => ({ ...l, step: s.label })));
  const tone = flow.state === "running" ? "run" : flow.state === "done" ? "ok" : flow.state === "failed" ? "err" : "idle";
  const word = flow.state === "done" ? t("servers.activity.done") : flow.state === "failed" ? t("servers.activity.failed") : flow.state === "canceled" ? t("servers.activity.canceled") : null;
  return (
    <section className={`k-sact ${tone}`} role="status" aria-live="polite" aria-label={flow.title} data-flow={flow.kind} data-state={flow.state}>
      <div className="k-sact-head">
        {running ? <Cones step={tick % 3} small /> : <span className={`k-sact-mark ${tone}`} aria-hidden>{flow.state === "done" ? <Check /> : flow.state === "failed" ? <X /> : <Minus />}</span>}
        <span className="k-sact-title">{flow.title}</span>
        {word && <span className={`k-word-sm k-tone-${flow.state === "done" ? "ok" : flow.state === "failed" ? "err" : "idle"}`}>{word}</span>}
        <span className="k-sact-time" title={t("servers.activity.timeTitle")}>{elapsed(took)}</span>
        {!running && <IconButton size="s" label={t("servers.activity.dismiss")} icon={<X />} onClick={() => dismiss(id)} />}
      </div>
      {flow.steps.length > 1 && (
        <ol className="k-sact-steps">
          {flow.steps.map((s, i) => (
            <li key={s.key} className={`k-sact-step ${s.state}`} aria-label={`${i + 1}. ${s.label}: ${t(`servers.activity.step.${s.state}`)}`}>
              <StepMark step={s} />
              <span>{s.label}</span>
              {s.state === "running" && s.startedAt && <span className="k-sact-step-time">{elapsed(now - s.startedAt)}</span>}
            </li>
          ))}
        </ol>
      )}
      {running && <div className="k-sact-line" title={newest ?? ""}>{newest ?? t("servers.activity.waiting")}</div>}
      {flow.state === "done" && (flow.doneText || flow.detail) && (
        <div className="k-sact-result">{flow.doneText}{flow.detail ? <span className="k-muted"> {flow.detail}</span> : null}</div>
      )}
      {flow.state === "failed" && (
        <div className="k-sact-error">
          <p>{flow.error?.message ?? t("servers.activity.noReason")}</p>
          <div className="k-sact-actions">
            <Button size="s" variant="primary" onClick={() => void retryFlow(qc, id)}>{t("servers.activity.tryAgain")}</Button>
            <Button size="s" onClick={() => nav(`/servers/${encodeURIComponent(id)}/overview`)}>{t("servers.activity.console")}</Button>
          </div>
        </div>
      )}
      {all.length > 0 && (
        <button type="button" className="k-link k-sact-more" aria-expanded={open} onClick={() => setOpen((o) => !o)}>
          {open ? <ChevronUp aria-hidden /> : <ChevronDown aria-hidden />}
          {open ? t("servers.activity.hideDetails") : t("servers.activity.showDetails", { count: all.length })}
        </button>
      )}
      {open && (
        <ol className="k-sact-lines" ref={linesRef}>
          {all.map((l, i) => (
            <li key={i}><span className="k-sact-at">{elapsed(l.at)}</span>{flow.steps.length > 1 && <span className="k-sact-of">{l.step}</span>}<span>{l.text}</span></li>
          ))}
        </ol>
      )}
    </section>
  );
}
