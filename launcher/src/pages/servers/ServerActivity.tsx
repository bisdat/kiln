// The server's activity panel, under the server head: the flow that runs or just ended (serverActions.ts). While it
// runs: the title, the time it has taken, its steps (stop, install, start again), the newest line the running step's
// task reported, and a way to end the step early when it has one (Cancel, or Stop waiting for a start); "Show details"
// lists every line. When it ends: the result, or why it failed with Try again and the console. A finished flow closes
// by itself after a while unless its details are open or it failed. Screen readers hear a short summary (the step, the
// result), not the timers or the log lines.
import { useQueryClient } from "@tanstack/react-query";
import { Check, ChevronDown, ChevronUp, Minus, X } from "lucide-react";
import { useEffect, useLayoutEffect, useRef, useState } from "react";
import { useNavigate } from "react-router-dom";
import { Button, Cones, IconButton, Spinner, toast } from "../../ui";
import { useT } from "../../i18n";
import { cancelStep, elapsed, markShown, retryFlow, useServerFlow, useServerFlows, type FlowStep } from "./serverActions";

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
  const setOpen = useServerFlows((s) => s.setOpen);
  const open = !!flow?.open;
  const running = flow?.state === "running";
  const now = useNow(running);
  const tick = Math.floor(now / 900);
  const linesRef = useRef<HTMLOListElement>(null);
  const seen = useRef(0);
  useEffect(() => markShown(id), [id]);
  // a finished flow closes by itself unless its details are open or it failed
  useEffect(() => {
    if (!flow || flow.state === "running" || open || flow.state === "failed") return;
    const left = CLOSE_AFTER_MS - (Date.now() - (flow.endedAt ?? Date.now()));
    const h = setTimeout(() => dismiss(id), Math.max(0, left));
    return () => clearTimeout(h);
  }, [flow, open, dismiss, id]);
  const all = flow ? flow.steps.flatMap((s) => s.lines.map((l) => ({ ...l, step: s.label }))) : [];
  // the details follow new lines while they are scrolled to the bottom; reading further up stays put
  useLayoutEffect(() => {
    const el = linesRef.current;
    if (!el) { seen.current = 0; return; }
    if (all.length > seen.current) {
      const atBottom = seen.current === 0 || el.scrollHeight - el.clientHeight - el.scrollTop < 40 + 24;
      if (atBottom) el.scrollTop = el.scrollHeight;
    }
    seen.current = all.length;
  }, [all.length, open]);
  if (!flow) return null;
  const took = (flow.endedAt ?? now) - flow.startedAt;
  const step = flow.steps.find((s) => s.state === "running");
  const cur = step ?? [...flow.steps].reverse().find((s) => s.lines.length);
  const newest = cur?.lines[cur.lines.length - 1]?.text;
  const tone = flow.state === "running" ? "run" : flow.state === "done" ? "ok" : flow.state === "failed" ? "err" : "idle";
  const word = flow.state === "done" ? t("servers.activity.done") : flow.state === "failed" ? t("servers.activity.failed") : flow.state === "canceled" ? t("servers.activity.canceled") : null;
  const summary = flow.state === "running" ? t("servers.activity.srRunning", { title: flow.title, step: step?.label ?? "" })
    : flow.state === "done" ? t("servers.activity.srDone", { title: flow.title, result: flow.doneText ?? "" })
    : flow.state === "failed" ? t("servers.activity.srFailed", { title: flow.title, reason: flow.error?.message ?? "" })
    : t("servers.activity.srCanceled", { title: flow.title });
  const stopEarly = running && step?.taskId && step.cancel;
  return (
    <section className={`k-sact ${tone}`} aria-label={flow.title} data-flow={flow.kind} data-state={flow.state}>
      <span className="k-sr-only" role="status" aria-live="polite">{summary}</span>
      <div className="k-sact-head">
        {running ? <Cones step={tick % 3} small /> : <span className={`k-sact-mark ${tone}`} aria-hidden>{flow.state === "done" ? <Check /> : flow.state === "failed" ? <X /> : <Minus />}</span>}
        <span className="k-sact-title">{flow.title}</span>
        {word && <span className={`k-word-sm k-tone-${flow.state === "done" ? "ok" : flow.state === "failed" ? "err" : "idle"}`}>{word}</span>}
        <span className="k-sact-time" aria-hidden title={t("servers.activity.timeTitle")}>{elapsed(took)}</span>
        {stopEarly && (
          <Button size="s" variant="ghost" onClick={() => cancelStep(id).catch((e) => toast.error(e))}>
            {step.cancel === "stopWaiting" ? t("servers.activity.stopWaiting") : t("servers.activity.cancel")}
          </Button>
        )}
        {!running && <IconButton size="s" label={t("servers.activity.dismiss")} icon={<X />} onClick={() => dismiss(id)} />}
      </div>
      {flow.steps.length > 1 && (
        <ol className="k-sact-steps">
          {flow.steps.map((s, i) => (
            <li key={s.key} className={`k-sact-step ${s.state}`} aria-label={`${i + 1}. ${s.label}: ${t(`servers.activity.step.${s.state}`)}`}>
              <StepMark step={s} />
              <span>{s.label}</span>
              {s.state === "running" && s.startedAt && <span className="k-sact-step-time" aria-hidden>{elapsed(now - s.startedAt)}</span>}
            </li>
          ))}
        </ol>
      )}
      {running && <div className="k-sact-line" aria-hidden title={newest ?? ""}>{newest ?? t("servers.activity.waiting")}</div>}
      {flow.state === "done" && (flow.doneText || flow.detail) && (
        <div className="k-sact-result">{flow.doneText}{flow.detail ? <span className="k-muted"> {flow.detail}</span> : null}</div>
      )}
      {flow.state === "failed" && (
        <div className="k-sact-error">
          {flow.detail && <p className="k-muted">{flow.detail}</p>}
          <p>{flow.error?.message ?? t("servers.activity.noReason")}</p>
          <div className="k-sact-actions">
            <Button size="s" variant="primary" onClick={() => void retryFlow(qc, id)}>{t("servers.activity.tryAgain")}</Button>
            <Button size="s" onClick={() => nav(`/servers/${encodeURIComponent(id)}/overview`)}>{t("servers.activity.console")}</Button>
          </div>
        </div>
      )}
      {all.length > 0 && (
        <button type="button" className="k-link k-sact-more" aria-expanded={open} onClick={() => setOpen(id, !open)}>
          {open ? <ChevronUp aria-hidden /> : <ChevronDown aria-hidden />}
          {open ? t("servers.activity.hideDetails") : t("servers.activity.showDetails", { count: all.length })}
        </button>
      )}
      {open && (
        <ol className="k-sact-lines" ref={linesRef} aria-label={t("servers.activity.linesLabel")}>
          {all.map((l, i) => (
            <li key={i}><span className="k-sact-at">{elapsed(l.at)}</span>{flow.steps.length > 1 && <span className="k-sact-of">{l.step}</span>}<span>{l.text}</span></li>
          ))}
        </ol>
      )}
    </section>
  );
}
