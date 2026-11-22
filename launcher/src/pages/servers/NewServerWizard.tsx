// The new-server wizard (servers revamp, mockup A screen 5), in place of the old New server dialog. Step 1, where and
// what: the folder, the name and id, the port, a copy of an existing install, the Kiln policy, and whether to install
// Kiln and start the server at the end. Install runs one flow (serverActions.ts, serverFlows.ts createFlow) whose steps
// are the wizard's steps 2 to 4: the install (server_create: the instance file, the copy, SteamCMD, the config, the
// Wine prefix), Install Kiln (the deploy step of Update Kiln) and Start, each shown as it runs with its task's lines,
// like a server's activity panel. Closing the dialog leaves the flow running: the new server's row in the list shows
// it, and New server opens on it again until it ends. A failed install says why; Back keeps what was typed.
import { useQueryClient } from "@tanstack/react-query";
import { ChevronDown, ChevronUp } from "lucide-react";
import { useEffect, useLayoutEffect, useRef, useState } from "react";
import { useNavigate } from "react-router-dom";
import { create } from "zustand";
import type { RuntimeBundle } from "../../api/commands";
import { Badge, Button, Cone, Field, Modal, NumberInput, PathPicker, Select, TextInput, Toggle, toast } from "../../ui";
import { useT } from "../../i18n";
import type { ServerInstanceX, ServerSpecX } from "./serverApi";
import { cancelStep, elapsed, markShown, retryFlow, runFlow, useServerFlows, type Flow, type FlowStep } from "./serverActions";
import { createFlow } from "./serverFlows";
import { newestServerBundle } from "./kilnUpdate";

/** kiln_manage::server::instance validate_name (also the kilnctl instance name). */
export const SERVER_ID = /^[a-z0-9][a-z0-9_-]{0,47}$/;
/** An absolute folder: "/srv/...", or "D:\\servers\\..." on Windows. */
export const ABSOLUTE = /^(\/|[A-Za-z]:[\\/])/;

/** "Ember Isles 2" → "ember-isles-2". */
export function idFromName(name: string): string {
  return name.toLowerCase().normalize("NFKD").replace(/[\u0300-\u036f]/g, "").replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 48).replace(/-+$/, "");
}

/** The first query port from 15637 (step 2) no server on this PC uses. */
export function freePort(servers: ServerInstanceX[]): number {
  const used = new Set(servers.filter((s) => s.kind !== "remote").flatMap((s) => [s.port, s.queryPort]));
  let p = 15637;
  while (used.has(p) && p < 65535) p += 2;
  return p;
}

interface Form {
  spec: ServerSpecX;
  /** the id was typed (else it follows the name) */
  idTyped: boolean;
  /** copy the game files from: "" none, a server's id, or "other" (a folder typed) */
  copy: string;
  other: string;
  kiln: boolean;
  start: boolean;
}

interface WizardStore {
  form: Form | null;
  /** the server whose install flow the wizard shows (null: step 1) */
  flowId: string | null;
  setForm: (f: Form | null) => void;
  setFlowId: (id: string | null) => void;
}

export const useWizard = create<WizardStore>((set) => ({
  form: null,
  flowId: null,
  setForm: (form) => set({ form }),
  setFlowId: (flowId) => set({ flowId }),
}));

const fresh = (servers: ServerInstanceX[], kiln: boolean): Form => ({
  spec: { id: "", name: "", dir: "", port: freePort(servers), reflinkFrom: null, policy: "verified-only", skipSteamcmd: false },
  idTyped: false, copy: "", other: "", kiln, start: false,
});

function useNow(on: boolean): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (!on) return;
    setNow(Date.now());
    const h = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(h);
  }, [on]);
  return now;
}

export function NewServerWizard({ open, onClose, servers, bundles }: { open: boolean; onClose: () => void; servers: ServerInstanceX[]; bundles: RuntimeBundle[] | undefined }) {
  const t = useT();
  const qc = useQueryClient();
  const nav = useNavigate();
  const { form, flowId, setForm, setFlowId } = useWizard();
  const flow = useServerFlows((s) => (flowId ? s.flows[flowId] : undefined));
  const newest = newestServerBundle(bundles);
  // each open starts at step 1 with a fresh form, unless the wizard's install still runs: then it opens on it
  useEffect(() => {
    if (!open) return;
    const w = useWizard.getState();
    if (w.flowId && useServerFlows.getState().flows[w.flowId]?.state === "running") return;
    setFlowId(null);
    setForm(fresh(servers, !!newest));
    // eslint-disable-next-line react-hooks/exhaustive-deps -- only when it opens (the list and the bundles change under it)
  }, [open]);
  const close = () => {
    if (flow && flow.state !== "running") setFlowId(null);
    onClose();
  };
  const f = form ?? fresh(servers, !!newest);
  const showing = flowId && flow ? flow : null;
  const set = (p: Partial<Form>) => setForm({ ...f, ...p });
  const spec = f.spec;
  const setSpec = (p: Partial<ServerSpecX>) => set({ spec: { ...spec, ...p } });
  // the checks (the backend's own: kiln_manage::server::instance::create)
  const idOk = SERVER_ID.test(spec.id);
  const idTaken = servers.find((s) => s.id === spec.id);
  const nameOk = spec.name.trim().length > 0 && spec.name.length <= 64;
  const dirOk = ABSOLUTE.test(spec.dir);
  const portOk = Number.isInteger(spec.port) && spec.port >= 1024 && spec.port <= 65535;
  const portUser = servers.find((s) => s.kind !== "remote" && (s.port === spec.port || s.queryPort === spec.port));
  const local = servers.filter((s) => s.kind !== "remote" && s.dir);
  const from = f.copy === "other" ? f.other.trim() : local.find((s) => s.id === f.copy)?.dir ?? "";
  const fromOk = !f.copy || ABSOLUTE.test(from);
  const ok = idOk && !idTaken && nameOk && dirOk && portOk && fromOk;
  const kilnV = f.kiln && newest ? newest.version : null;
  const plan = [f.copy ? (spec.skipSteamcmd ? t("servers.wizard.stepCopyOnly") : t("servers.wizard.stepCopy")) : t("servers.wizard.stepCreate"), kilnV && t("servers.activity.stepDeploy", { version: kilnV }), f.start && t("servers.activity.stepStart")]
    .filter(Boolean) as string[];
  const install = () => {
    if (!ok) return;
    const s: ServerSpecX = { ...spec, name: spec.name.trim(), reflinkFrom: f.copy ? from : null, skipSteamcmd: !!f.copy && !!spec.skipSteamcmd };
    setForm({ ...f, spec: s });
    setFlowId(s.id);
    void runFlow(qc, s.id, createFlow(qc, { spec: s, kiln: kilnV, start: f.start }));
  };
  let footer;
  if (!showing) {
    footer = <>
      <Button variant="ghost" onClick={close}>{t("common.cancel")}</Button>
      <Button variant="primary" onClick={install} disabled={!ok}>{t("common.install")}</Button>
    </>;
  } else {
    const run = showing.steps.find((s) => s.state === "running");
    const failedAt = showing.steps.find((s) => s.state === "failed" || s.state === "canceled");
    const atCreate = failedAt?.key === "create";
    const openServer = () => { close(); nav(`/servers/${encodeURIComponent(showing.serverId)}/overview`); };
    footer = showing.state === "running" ? <>
      <span className="k-wiz-foot-note">{t("servers.wizard.keepsGoing")}</span>
      {run?.cancel && run.taskId && (
        <Button variant="ghost" onClick={() => cancelStep(showing.serverId).catch((e) => toast.error(e))}>
          {run.cancel === "stopWaiting" ? t("servers.activity.stopWaiting") : t("servers.wizard.cancelInstall")}
        </Button>
      )}
      <Button onClick={close}>{t("common.close")}</Button>
    </> : showing.state === "done" ? <>
      <Button variant="ghost" onClick={close}>{t("common.close")}</Button>
      <Button variant="primary" onClick={openServer}>{t("servers.wizard.open")}</Button>
    </> : atCreate || !failedAt ? <>
      <Button variant="ghost" onClick={close}>{t("common.close")}</Button>
      <Button variant="primary" onClick={() => setFlowId(null)}>{t("common.back")}</Button>
    </> : <>
      <Button variant="ghost" onClick={close}>{t("common.close")}</Button>
      <Button onClick={openServer}>{t("servers.wizard.open")}</Button>
      <Button variant="primary" onClick={() => void retryFlow(qc, showing.serverId)}>{t("servers.activity.tryAgain")}</Button>
    </>;
  }
  return (
    <Modal open={open} onClose={close} title={showing ? showing.title : t("servers.wizard.title")} width={660} footer={footer}>
      <Stepper flow={showing} plan={{ kiln: showing ? showing.steps.some((s) => s.key === "deploy") : !!kilnV, start: showing ? showing.steps.some((s) => s.key === "start") : f.start }} />
      {showing ? <Progress flow={showing} halfMade={servers.some((s) => s.id === showing.serverId)} /> : (
        <div className="k-wiz-form">
          <Field label={t("servers.list.name")} hint={spec.name.length > 64 ? undefined : t("servers.list.nameHint")} error={spec.name.length > 64 ? t("servers.wizard.nameLong") : undefined} htmlFor="wiz-name">
            <TextInput id="wiz-name" data-autofocus value={spec.name} placeholder={t("servers.list.namePlaceholder")}
              onChange={(e) => { const name = e.target.value; setSpec({ name, ...(f.idTyped ? {} : { id: idFromName(name) }) }); }} />
          </Field>
          <Field label={t("servers.list.id")} htmlFor="wiz-id" hint={t("servers.list.idHint")}
            error={spec.id && !idOk ? t("servers.list.idBad") : idTaken ? t("servers.wizard.idTaken", { name: idTaken.name }) : undefined}>
            <TextInput id="wiz-id" mono value={spec.id} placeholder="ember-isles" invalid={!!spec.id && (!idOk || !!idTaken)}
              onChange={(e) => set({ idTyped: e.target.value !== "", spec: { ...spec, id: e.target.value } })} />
          </Field>
          <div className="full">
            <Field label={t("servers.list.folder")} htmlFor="wiz-dir" hint={t("servers.list.folderHint")} error={spec.dir && !dirOk ? t("servers.wizard.folderBad") : undefined}>
              <PathPicker id="wiz-dir" value={spec.dir} onChange={(dir: string) => setSpec({ dir: dir.trim() })} directory placeholder="/home/player/srv/enshrouded/ember-isles" />
            </Field>
          </div>
          <div className="full k-stack" style={{ gap: 8 }}>
            <Field label={t("servers.list.copyFrom")} htmlFor="wiz-copy" hint={t("servers.list.copyFromHint")}>
              <Select id="wiz-copy" value={f.copy} onChange={(copy) => set({ copy })}
                options={[{ value: "", label: t("servers.wizard.copyNone") }, ...local.map((s) => ({ value: s.id, label: t("servers.wizard.copyServer", { name: s.name, dir: s.dir ?? "" }) })),
                  { value: "other", label: t("servers.wizard.copyOther") }]} />
            </Field>
            {f.copy === "other" && (
              <Field label={t("servers.wizard.copyFolder")} htmlFor="wiz-from" error={f.other && !fromOk ? t("servers.wizard.folderBad") : undefined}>
                <PathPicker id="wiz-from" value={f.other} onChange={(other: string) => set({ other })} directory />
              </Field>
            )}
            {!!f.copy && <Toggle checked={!!spec.skipSteamcmd} onChange={(skipSteamcmd) => setSpec({ skipSteamcmd })} label={t("servers.list.skipSteamcmd")} hint={t("servers.list.skipSteamcmdHint")} />}
          </div>
          <Field label={t("servers.list.port")} htmlFor="wiz-port" error={!portOk ? t("servers.wizard.portBad") : undefined}
            hint={portUser ? <span className="k-tone-warn">{t("servers.wizard.portTaken", { name: portUser.name })}</span> : t("servers.list.portHint")}>
            <div className="k-input-group">
              {/* no min/max on the box: every whole number reaches the check, which says what is wrong and holds Install */}
              <NumberInput id="wiz-port" integer value={spec.port} invalid={!portOk} onCommit={(port) => setSpec({ port })} style={{ maxWidth: 130 }} />
              {portOk && <Badge tone={portUser ? "warn" : "ok"}>{portUser ? t("servers.wizard.portTakenWord") : t("servers.wizard.portFree")}</Badge>}
            </div>
          </Field>
          <Field label={t("servers.list.policy")} htmlFor="wiz-policy">
            <Select id="wiz-policy" value={spec.policy ?? "verified-only"} onChange={(policy) => setSpec({ policy })}
              options={[{ value: "verified-only", label: t("servers.list.policyVerified") }, { value: "any", label: t("servers.list.policyAny") }]} />
          </Field>
          <div className="full k-stack" style={{ gap: 10 }}>
            <Toggle checked={!!newest && f.kiln} disabled={!newest} onChange={(kiln) => set({ kiln })}
              label={newest ? t("servers.wizard.kilnToggle", { version: newest.version }) : t("servers.wizard.kilnNone")}
              hint={newest ? t("servers.wizard.kilnHint") : t("servers.wizard.kilnNoneHint")} />
            <Toggle checked={f.start} onChange={(start) => set({ start })} label={t("servers.wizard.startToggle")} />
          </div>
          <ol className="k-sact-plan full" aria-label={t("servers.activity.planLabel")}>
            {plan.map((s, i) => <li key={i}>{s}</li>)}
          </ol>
        </div>
      )}
    </Modal>
  );
}

/** 1 Where and what, 2 Install, 3 Install Kiln, 4 Start: done, the one now, failed, or not chosen. */
function Stepper({ flow, plan }: { flow: Flow | null; plan: { kiln: boolean; start: boolean } }) {
  const t = useT();
  const of = (key: string): string => {
    const s = flow?.steps.find((x) => x.key === key);
    if (!s) return "";
    return s.state === "running" ? "on" : s.state === "done" ? "done" : s.state === "failed" || s.state === "canceled" ? "failed" : "";
  };
  const items: [string, string, boolean][] = [
    [t("servers.wizard.s1"), flow ? "done" : "on", true],
    [t("servers.wizard.s2"), of("create"), true],
    [t("servers.wizard.s3"), of("deploy"), plan.kiln],
    [t("servers.wizard.s4"), of("start"), plan.start],
  ];
  return (
    <ol className="k-wiz-stepper" aria-label={t("servers.wizard.stepper")}>
      {items.map(([label, cls, chosen], i) => (
        <li key={i} className={chosen ? cls : "off"} aria-current={cls === "on" ? "step" : undefined}>
          <i aria-hidden />{i + 1} {label}{!chosen && <small> {t("servers.wizard.notChosen")}</small>}
        </li>
      ))}
    </ol>
  );
}

function stepSub(s: FlowStep, t: ReturnType<typeof useT>): string {
  const line = s.lines[s.lines.length - 1]?.text;
  if (s.state === "pending") return t("servers.activity.step.pending");
  if (s.state === "skipped") return t("servers.activity.step.skipped");
  if (s.state === "canceled") return t("servers.activity.step.canceled");
  return line ?? (s.state === "running" ? t("servers.activity.waiting") : t(`servers.activity.step.${s.state}`));
}

/** The install's steps with their cones, lines and times; the clock; the result or why it failed. */
function Progress({ flow, halfMade }: { flow: Flow; halfMade: boolean }) {
  const t = useT();
  const running = flow.state === "running";
  const now = useNow(running);
  const [open, setOpen] = useState(false);
  const linesRef = useRef<HTMLOListElement>(null);
  useEffect(() => markShown(flow.serverId), [flow.serverId]);
  const all = flow.steps.flatMap((s) => s.lines.map((l) => ({ ...l, step: s.label })));
  useLayoutEffect(() => { const el = linesRef.current; if (el) el.scrollTop = el.scrollHeight; }, [all.length, open]);
  const run = flow.steps.find((s) => s.state === "running");
  const newest = run?.lines[run.lines.length - 1]?.text;
  const failedAt = flow.steps.find((s) => s.state === "failed");
  const name = flow.title;
  const summary = running ? t("servers.activity.srRunning", { title: flow.title, step: run?.label ?? "" })
    : flow.state === "done" ? t("servers.activity.srDone", { title: flow.title, result: flow.doneText ?? "" })
    : flow.state === "failed" ? t("servers.activity.srFailed", { title: flow.title, reason: flow.error?.message ?? "" })
    : t("servers.activity.srCanceled", { title: flow.title });
  return (
    <section className="k-wiz-progress" aria-label={name} data-state={flow.state}>
      <span className="k-sr-only" role="status" aria-live="polite">{summary}</span>
      <ol className="k-wiz-steps">
        {flow.steps.map((s, i) => {
          const took = s.startedAt ? (s.endedAt ?? now) - s.startedAt : null;
          return (
            <li key={s.key} className={`k-wiz-step ${s.state}`} aria-label={`${i + 1}. ${s.label}: ${t(`servers.activity.step.${s.state}`)}`}>
              <span className="k-wiz-cone" aria-hidden><Cone state={s.state === "done" ? "done" : s.state === "running" ? "bend" : ""} /></span>
              <span className="k-wiz-step-name">
                <span>{s.label}</span>
                <small title={stepSub(s, t)}>{stepSub(s, t)}{s.state === "running" && s.progress != null && s.progress > 0 ? ` (${Math.round(s.progress * 100)}%)` : ""}</small>
              </span>
              <span className="k-wiz-step-time k-mono" aria-hidden>{took != null && s.state !== "pending" && s.state !== "skipped" ? elapsed(took) : ""}</span>
            </li>
          );
        })}
      </ol>
      {running && (
        <div className="k-wiz-clock" aria-hidden>
          <span className="k-wiz-clock-n">{elapsed(now - flow.startedAt)}</span>
          <span className="k-wiz-clock-l"><b>{t("servers.wizard.elapsed")}</b><span className="k-sact-line" title={newest ?? ""}>{newest ?? t("servers.activity.waiting")}</span></span>
        </div>
      )}
      {flow.state === "done" && (
        <p className="k-wiz-result">{flow.steps.some((s) => s.key === "start") ? t("servers.wizard.readyRunning") : t("servers.wizard.ready")}
          <span className="k-muted"> {t("servers.wizard.readyHint", { time: elapsed((flow.endedAt ?? now) - flow.startedAt) })}</span></p>
      )}
      {flow.state === "failed" && (
        <div className="k-sact-error k-wiz-error">
          <p className="k-wiz-error-at">{t("servers.wizard.failedAt", { step: failedAt?.label ?? "" })}</p>
          <p>{flow.error?.message ?? t("servers.activity.noReason")}</p>
          {failedAt?.key === "create" && halfMade && <p className="k-muted">{t("servers.wizard.halfMade", { id: flow.serverId })}</p>}
        </div>
      )}
      {flow.state === "canceled" && <p className="k-wiz-result k-muted">{t("servers.activity.srCanceled", { title: flow.title })}</p>}
      {all.length > 0 && (
        <button type="button" className="k-link k-sact-more" aria-expanded={open} onClick={() => setOpen(!open)}>
          {open ? <ChevronUp aria-hidden /> : <ChevronDown aria-hidden />}
          {open ? t("servers.activity.hideDetails") : t("servers.activity.showDetails", { count: all.length })}
        </button>
      )}
      {open && (
        <ol className="k-sact-lines" ref={linesRef} aria-label={t("servers.activity.linesLabel")}>
          {all.map((l, i) => <li key={i}><span className="k-sact-at">{elapsed(l.at)}</span><span className="k-sact-of">{l.step}</span><span>{l.text}</span></li>)}
        </ol>
      )}
    </section>
  );
}
