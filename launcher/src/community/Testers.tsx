// The tester programme (WIREFRAMES D15, K12). Not a tester: why the team needs testers, what this PC can test,
// Volunteer (a dialog that shows first exactly what the launcher would share; sent only on its button, then the
// launcher can switch to test builds through the existing channel switch). A tester: the current test build's
// checklist with only this hardware's steps, a result per step (Worked, Didn't work, Couldn't try, one line), Send
// results with a support report for the team only, whether the team has read them, and Stop testing.
import { MoreHorizontal } from "lucide-react";
import { useState } from "react";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { Hardware, StepResult, TestPlan, TestStep } from "../api/commands";
import { useUpdate } from "../api/update";
import { runSwitch } from "../shell/ChannelSwitch";
import { t as tt, useT } from "../i18n";
import { Button, Card, EmptyState, KeyValue, Menu, Modal, StatusPill, TextInput, fmtBytes, toast } from "../ui";
import { useGame } from "../api/hooks";
import { DeckButton, Dialog, Row, Word } from "../deck/ui";
import { ago, cq, errKind, openInDiscord, refreshCommunity, useCommunity, useCommunityUi, useTester } from "./state";
import { discordWords } from "./parts";
import { Pick } from "./DeckCommunity";
import { DeckSignIn } from "./SignIn";
import { TypeDialog } from "./DeckSend";

const RESULTS: StepResult[] = ["worked", "failed", "skipped"];

/** Send results failed: Discord's refusal or rate limit, and frostdev.io not answering, in the player's language (the
 *  backend's own sentence otherwise). The checklist keeps every result for the next try. */
function resultsFailed(e: unknown) {
  const d = (e as { detail?: Record<string, unknown> })?.detail ?? {};
  const k = errKind(e);
  if (k === "discord") { const w = discordWords(d, "results"); return toast.error({ code: "refused", message: w.text }, w.title); }
  if (k === "relay") return toast.error({ code: "network", message: tt("community.signIn.relay") }, tt("community.send.err.relayTitle"));
  return toast.error(e);
}
export const resultTone = (r: StepResult | null) => (r === "worked" ? "ok" : r === "failed" ? "err" : r === "skipped" ? "warn" : "idle") as "ok" | "err" | "warn" | "idle";
const untilDate = (iso: string) => new Date(iso).toLocaleDateString([], { weekday: "long", day: "numeric", month: "long" });
/** Shown for a fact the launcher could not read. */
const fact = (s: string) => s || tt("community.tester.unknown");

/** The support report a tester attaches to the results, prepared while it is ticked so the checklist shows its size
 *  and files before Send results (the same report as Send to Kiln support's, for the team only), and sent as shown. */
function useAttached(on: boolean) {
  const { game } = useGame();
  return useQuery({ queryKey: ["community", "prepare", "tester"], queryFn: () => api.communityReportPrepare(game ? { game: game.id } : null, "community"),
    enabled: on, staleTime: 25 * 60_000, gcTime: 25 * 60_000, retry: false });
}

/** Volunteer, then the switch to test builds when ticked (the existing channel switch, as Settings does it). */
function useVolunteer() {
  const qc = useQueryClient();
  const update = useUpdate();
  const canSwitch = !!update && update.channel !== "beta";
  const volunteer = async (beta: boolean) => {
    await api.testerJoin(beta);
    await refreshCommunity(qc);
    toast.ok(tt("community.tester.done"), tt("community.tester.doneDetail"));
    if (beta && update && canSwitch) await runSwitch(qc, update, "beta", false).catch(() => null);
  };
  return { volunteer, canSwitch };
}

async function stopTesting(qc: ReturnType<typeof useQueryClient>) {
  try {
    await api.testerLeave();
    await refreshCommunity(qc);
    toast.ok(tt("community.tester.leaveDone"));
  } catch (e) { toast.error(e); }
}

export function TesterSection({ signedIn, net }: { signedIn: boolean; net: boolean }) {
  const t = useT();
  const q = useTester();
  const [volunteer, setVolunteer] = useState(false);
  const ts = q.data;
  if (!ts) return null;
  if (!ts.tester) {
    return (
      <Card title={t("community.tester.title")} data-testid="kc-tester">
        <p className="kc-p">{t("community.tester.pitch")}</p>
        <p className="kc-p">{ts.hardware.covers.length ? <><b className="k-tone-ember">{t("community.tester.yours", { what: ts.hardware.covers.join(", ") })}</b> {t("community.tester.joinText")}</> : t("community.tester.any")}</p>
        <div><Button onClick={() => setVolunteer(true)} disabled={!net} data-testid="kc-volunteer-open">{t("community.tester.volunteer")}</Button></div>
        {volunteer && <VolunteerDialog hw={ts.hardware} signedIn={signedIn} onClose={() => setVolunteer(false)} />}
      </Card>
    );
  }
  if (!ts.plan) return <NoPlan net={net} />;
  return <Checklist plan={ts.plan} net={net} />;
}

/** A tester while the relay serves no test plan: Stop testing is here then (with a plan, in the checklist's ···). */
function NoPlan({ net }: { net: boolean }) {
  const t = useT();
  const qc = useQueryClient();
  const [busy, setBusy] = useState(false);
  const leave = () => { setBusy(true); void stopTesting(qc).finally(() => setBusy(false)); };
  return (
    <Card title={t("community.tester.title")} data-testid="kc-tester"
      actions={<button type="button" className="k-link k-tone-err" disabled={!net || busy} onClick={leave} data-testid="kc-tester-leave">{t("community.tester.leave")}</button>}>
      <p className="kc-p">{t("community.tester.you")}</p>
      <EmptyState title={t("community.tester.plan.noPlan")} />
    </Card>
  );
}

function HardwareFacts({ hw }: { hw: Hardware }) {
  const t = useT();
  return (
    <KeyValue rows={[
      [t("community.tester.gpu"), fact(hw.gpu)], [t("community.tester.driver"), fact(hw.driver)], [t("community.tester.vram"), fact(hw.vram)],
      [t("community.tester.os"), fact(hw.os)], [t("community.tester.runner"), fact(hw.runner)],
      hw.covers.length > 0 && [t("community.tester.covers"), <b className="k-tone-ember">{hw.covers.join(", ")}</b>],
    ]} />
  );
}

function VolunteerDialog({ hw, signedIn, onClose }: { hw: Hardware; signedIn: boolean; onClose: () => void }) {
  const t = useT();
  const { volunteer, canSwitch } = useVolunteer();
  const [beta, setBeta] = useState(true);
  const [busy, setBusy] = useState(false);
  const go = async () => {
    if (!signedIn) { onClose(); useCommunityUi.getState().openSignIn(); return; }
    setBusy(true);
    try { await volunteer(beta && canSwitch); onClose(); } catch (e) { toast.error(e); } finally { setBusy(false); }
  };
  return (
    <Modal open onClose={onClose} title={t("community.tester.volunteer")} width={580} locked={busy}
      footer={<>
        <Button onClick={onClose} disabled={busy}>{t("common.cancel")}</Button>
        <Button variant="primary" loading={busy} onClick={go} data-testid="kc-volunteer">{signedIn ? t("community.tester.send") : t("community.account.signIn")}</Button>
      </>}>
      <div className="k-stack" style={{ gap: 14 }} data-testid="kc-volunteer-dialog">
        <p className="kc-p">{t("community.tester.joinText")}</p>
        <div className="k-label">{t("community.tester.hwTitle")}</div>
        <HardwareFacts hw={hw} />
        <span className="k-hint">{t("community.tester.hwHint")}</span>
        {canSwitch && (
          <label className="kc-tick"><input type="checkbox" className="k-check" checked={beta} onChange={(e) => setBeta(e.target.checked)} data-testid="kc-beta" />
            <span><span>{t("community.tester.beta")}</span><br /><span className="k-hint">{t("community.tester.betaHint")}</span></span></label>
        )}
        {!signedIn && <span className="k-hint k-tone-warn">{t("community.tester.signInFirst")}</span>}
      </div>
    </Modal>
  );
}

function Checklist({ plan, net }: { plan: TestPlan; net: boolean }) {
  const t = useT();
  const qc = useQueryClient();
  const [openStep, setOpenStep] = useState<string | null>(plan.steps.find((s) => !s.result)?.id ?? null);
  const [attach, setAttach] = useState(true);
  const [busy, setBusy] = useState(false);
  const [files, setFiles] = useState(false);
  const report = useAttached(attach && !plan.sent);
  const steps = plan.steps;
  const done = steps.filter((s) => s.result).length;
  const setResult = (s: TestStep, result: StepResult | "clear" | undefined, note?: string) =>
    api.testerResultSet(s.id, result, note).then(async (p) => {
      // a read of the tester's state still on its way would land after this change and undo it on screen
      await qc.cancelQueries({ queryKey: cq.tester });
      if (p) qc.setQueryData(cq.tester, (old: unknown) => ({ ...(old as object), plan: p }));
    }).catch((e) => toast.error(e));
  const send = async () => {
    setBusy(true);
    try {
      await api.testerResultsSend(attach, attach ? report.data?.prepared : undefined);
      qc.removeQueries({ queryKey: ["community", "prepare", "tester"] });
      await qc.invalidateQueries({ queryKey: cq.tester });
      toast.ok(t("community.tester.plan.sentToast"), t("community.tester.plan.sentToastDetail"));
    } catch (e) { resultsFailed(e); } finally { setBusy(false); }
  };
  const sent = plan.sent;
  return (
    <Card title={t("community.tester.plan.title", { build: plan.build })} sub={t("community.tester.plan.say", { done, count: steps.length, until: untilDate(plan.until) })} data-testid="kc-checklist"
      actions={<div className="k-row" style={{ gap: 10 }}>
        <button type="button" className="k-link" onClick={() => openInDiscord(plan.url)}>{t("community.tester.plan.openPost")}</button>
        <Menu label={t("community.tester.plan.more")} icon={<MoreHorizontal />} size="s" items={[{ label: t("community.tester.leave"), danger: true, disabled: !net, onSelect: () => void stopTesting(qc) }]} />
      </div>}>
      {sent && (
        <p className="kc-p" data-testid="kc-results-state">
          {sent.read ? <><StatusPill tone="ok">{t("community.posts.status.repliedSeen")}</StatusPill> {t("community.tester.plan.read", { name: sent.read.by, ago: ago(sent.read.at) })}{sent.replied ? ` ${t("community.tester.plan.replied")}` : ""}</>
            : <><StatusPill tone="idle">{t("community.posts.status.waiting")}</StatusPill> {t("community.tester.plan.sent", { ago: ago(sent.at) })}</>}
        </p>
      )}
      <ol className="kc-steps">
        {steps.map((s, i) => (
          <li key={s.id} className={"kc-step" + (openStep === s.id ? " open" : "")} data-step={s.id}>
            <button type="button" className="kc-step-head" aria-expanded={openStep === s.id} onClick={() => setOpenStep(openStep === s.id ? null : s.id)}>
              <span className="kc-step-n">{i + 1}</span>
              <span className="kc-step-title">{s.title}{s.for && <span className="k-hint"> · {t("community.tester.plan.for", { what: s.for.label })}</span>}</span>
              <StatusPill tone={resultTone(s.result)}>{t(`community.tester.plan.result.${s.result ?? "none"}`)}</StatusPill>
            </button>
            {openStep === s.id && (
              <div className="kc-step-body">
                <ol className="kc-how">{s.how.map((h) => <li key={h}>{h}</li>)}</ol>
                <div className="kc-results" role="radiogroup" aria-label={s.title}>
                  {RESULTS.map((r) => (
                    <button key={r} type="button" role="radio" aria-checked={s.result === r} className={"kc-result" + (s.result === r ? ` on ${resultTone(r)}` : "")}
                      disabled={!!sent} onClick={() => setResult(s, s.result === r ? "clear" : r)}>{t(`community.tester.plan.result.${r}`)}</button>
                  ))}
                </div>
                <TextInput aria-label={t("community.tester.plan.note")} placeholder={t("community.tester.plan.notePh")} defaultValue={s.note} disabled={!!sent} maxLength={140}
                  onBlur={(e) => { if (e.target.value !== s.note) setResult(s, undefined, e.target.value); }} />
              </div>
            )}
          </li>
        ))}
      </ol>
      {plan.hidden > 0 && <span className="k-hint">{t("community.tester.plan.hidden", { count: plan.hidden })}</span>}
      {!sent && (
        <div className="k-row" style={{ gap: 14 }}>
          <Button onClick={send} loading={busy} disabled={!net || done < steps.length || (attach && !report.data)} data-testid="kc-send-results">{t("community.tester.plan.sendResults")}</Button>
          <label className="kc-tick"><input type="checkbox" className="k-check" checked={attach} onChange={(e) => setAttach(e.target.checked)} data-testid="kc-attach" />
            <span>{t("community.tester.plan.attach")} <span className="k-hint">{t("community.tester.plan.attachHint")}</span></span></label>
          {done < steps.length && <span className="k-hint">{t("community.tester.plan.untried")}</span>}
        </div>
      )}
      {!sent && attach && (
        <div className="kc-block" data-testid="kc-attached">
          <div className="kc-block-head">
            <span className="k-label">{t("community.send.check.zip")}</span>
            <span className="k-hint">{report.data ? t("community.send.check.zipHint", { size: fmtBytes(report.data.size), count: report.data.files.length }) : t("community.send.checking")}</span>
            {report.data && <button type="button" className="k-link kc-files-toggle" onClick={() => setFiles(!files)}>{files ? t("community.send.check.hideFiles") : t("community.send.check.showFiles")}</button>}
          </div>
          {files && report.data && <ul className="kc-files" data-testid="kc-attached-files">{report.data.files.map((f) => <li key={f.path}><span className="k-mono">{f.path}</span><span className="k-muted">{fmtBytes(f.size)}</span></li>)}</ul>}
          <span className="k-hint">{t("community.send.check.redacted")}</span>
        </div>
      )}
    </Card>
  );
}

// ---------------------------------------------------------------- the Deck (K12)

export function DeckTesterRow() {
  const t = useT();
  const s = useCommunity().data;
  const tester = useTester();
  const [open, setOpen] = useState(false);
  const plan = tester.data?.plan ?? null;
  const on = !!tester.data?.tester;
  return (
    <>
      <Row id="tester" title={on && plan ? t("community.tester.plan.title", { build: plan.build }) : t("community.tester.title")}
        sub={on && plan ? t("community.deck.testerSubOn", { done: plan.steps.filter((x) => x.result).length, count: plan.steps.length }) : t("community.deck.testerSub")}
        end={on ? <Word tone="ember">{t("community.account.tester")}</Word> : undefined} a={t("common.open")} onClick={() => setOpen(true)} />
      {open && <DeckTester onClose={() => setOpen(false)} signedIn={!!s?.account} />}
    </>
  );
}

function DeckTester({ onClose, signedIn }: { onClose: () => void; signedIn: boolean }) {
  const t = useT();
  const qc = useQueryClient();
  const q = useTester();
  const { volunteer, canSwitch } = useVolunteer();
  const [step, setStep] = useState<TestStep | null>(null);
  const [why, setWhy] = useState(false);
  const [typing, setTyping] = useState(false);
  const [signIn, setSignIn] = useState(false);
  const [beta, setBeta] = useState(true);
  const [attach, setAttach] = useState(true);
  const [files, setFiles] = useState(false);
  const ts = q.data;
  const report = useAttached(attach && !!ts?.plan && !ts.plan.sent);
  if (signIn) return <DeckSignIn onClose={() => setSignIn(false)} />;
  if (!ts) return null;
  const hw = ts.hardware;
  if (!ts.tester || !ts.plan) {
    if (ts.tester) {
      return (
        <Dialog key="noplan" name="tester-noplan" title={t("community.tester.title")} wide onClose={onClose} backLabel={t("common.close")}
          buttons={<><DeckButton onClick={() => void stopTesting(qc).then(onClose)}>{t("community.tester.leave")}</DeckButton><DeckButton primary onClick={onClose}>{t("common.ok")}</DeckButton></>}>
          <p className="deck-say">{t("community.tester.you")} {t("community.tester.plan.noPlan")}</p>
        </Dialog>
      );
    }
    return (
      <Dialog key="volunteer" name="tester-volunteer" title={t("community.tester.volunteer")} wide onClose={onClose}
        buttons={signedIn
          ? <DeckButton primary onClick={() => volunteer(beta && canSwitch).catch((e) => toast.error(e))}>{t("community.tester.send")}</DeckButton>
          : <DeckButton primary onClick={() => setSignIn(true)}>{t("community.account.signIn")}</DeckButton>}>
        <div className="deck-scroll" data-testid="deck-volunteer">
          <p>{t("community.tester.pitch")}</p>
          <p className="deck-label">{t("community.tester.hwTitleDeck")}</p>
          <dl className="kdc-hw" tabIndex={0} data-deck-a="-" data-deck-id="hw">
            {([["gpu", hw.gpu], ["driver", hw.driver], ["vram", hw.vram], ["os", hw.os], ["runner", hw.runner], ["covers", hw.covers.join(", ")]] as const)
              .filter(([k, v]) => k !== "covers" || v).map(([k, v]) => <div key={k}><dt>{t(`community.tester.${k}`)}</dt><dd>{fact(v)}</dd></div>)}
          </dl>
          <p className="deck-dim">{t("community.tester.hwHint")}</p>
          {canSwitch && <Pick id="beta" title={t("community.tester.beta")} sub={t("community.tester.betaHint")} on={beta} end={<Word tone={beta ? "ok" : "idle"}>{beta ? t("community.deck.on") : t("community.deck.off")}</Word>} a={t("community.deck.toggle")} onClick={() => setBeta(!beta)} />}
          {!signedIn && <p className="deck-note">{t("community.tester.signInFirst")}</p>}
        </div>
      </Dialog>
    );
  }
  const plan = ts.plan;
  const set = (s: TestStep, result: StepResult | "clear" | undefined, note?: string) =>
    api.testerResultSet(s.id, result, note).then(() => qc.invalidateQueries({ queryKey: cq.tester })).catch((e) => toast.error(e));
  if (files && report.data) {
    return (
      <Dialog key="files" name="tester-files" title={t("community.deck.whatIsSent")} wide onClose={() => setFiles(false)} backLabel={t("common.back")}
        buttons={<DeckButton primary onClick={() => setFiles(false)}>{t("common.ok")}</DeckButton>}>
        <div className="deck-scroll kdc-sent-list" tabIndex={0} data-deck-a="-" data-deck-id="sent-list" data-testid="deck-attached-files">
          <p className="deck-label">{t("community.send.check.zip")} · {fmtBytes(report.data.size)}</p>
          <ul>{report.data.files.map((f) => <li key={f.path} className="deck-mono">{f.path}</li>)}</ul>
          <p className="deck-dim">{t("community.send.check.redacted")}</p>
        </div>
      </Dialog>
    );
  }
  if (step && typing) return <TypeDialog value={step.note} max={140} onDone={(v) => { void set(step, undefined, v); setTyping(false); setWhy(false); setStep(null); }} />;
  if (step && why) {
    const notes = ["crashed", "wrong", "slow", "missing"] as const;
    return (
      <Dialog key="why" name="tester-why" title={t("community.deck.noteWhy.title")} onClose={() => setWhy(false)} backLabel={t("common.back")}>
        <div className="deck-list" data-testid="deck-why">
          {notes.map((n, i) => <Pick key={n} id={`why-${n}`} first={i === 0} title={t(`community.deck.noteWhy.${n}`)} a={t("community.deck.pick")}
            onClick={() => { void set(step, undefined, t(`community.deck.noteWhy.${n}`)); setWhy(false); setStep(null); }} />)}
          <Pick id="why-type" title={t("community.deck.noteWhy.type")} a={t("community.deck.type")} onClick={() => setTyping(true)} />
        </div>
      </Dialog>
    );
  }
  if (step) {
    const cur = plan.steps.find((x) => x.id === step.id) ?? step;
    return (
      <Dialog key={`step-${step.id}`} name="tester-step" title={cur.title} wide onClose={() => setStep(null)} backLabel={t("common.back")}>
        <div className="deck-scroll" data-testid="deck-step">
          {cur.for && <p className="deck-label">{t("community.tester.plan.for", { what: cur.for.label })}</p>}
          <ol className="kdc-steps">{cur.how.map((h) => <li key={h}>{h}</li>)}</ol>
          <div className="deck-row-buttons kdc-results">
            {RESULTS.map((r, i) => <DeckButton key={r} id={`r-${r}`} autoFocus={cur.result ? cur.result === r : i === 0} className={cur.result === r ? `on kdc-r-${resultTone(r)}` : ""}
              onClick={() => { void set(cur, r).then(() => { if (r === "worked") setStep(null); else setWhy(true); }); }}>{t(`community.tester.plan.result.${r}`)}</DeckButton>)}
          </div>
          {cur.note && <p className="deck-dim">{cur.note}</p>}
        </div>
      </Dialog>
    );
  }
  const done = plan.steps.filter((x) => x.result).length;
  return (
    <Dialog key="plan" name="tester-plan" title={t("community.tester.plan.title", { build: plan.build })} wide onClose={onClose} backLabel={t("common.close")}
      actions={[{ button: "x", label: t("community.tester.leave"), run: () => void stopTesting(qc).then(onClose) }]}
      buttons={plan.sent ? <DeckButton primary onClick={onClose}>{t("common.ok")}</DeckButton>
        : <DeckButton primary disabled={done < plan.steps.length || (attach && !report.data)} onClick={() => api.testerResultsSend(attach, attach ? report.data?.prepared : undefined)
            .then(() => { qc.removeQueries({ queryKey: ["community", "prepare", "tester"] }); return qc.invalidateQueries({ queryKey: cq.tester }); })
            .then(() => toast.ok(t("community.tester.plan.sentToast"))).catch(resultsFailed)}>{t("community.tester.plan.sendResults")}</DeckButton>}>
      <div className="deck-scroll" data-testid="deck-plan">
        {plan.sent ? <p className="deck-line" data-testid="deck-results-state">{plan.sent.read ? <><Word tone="ok">{t("community.posts.status.repliedSeen")}</Word> <span>{t("community.tester.plan.read", { name: plan.sent.read.by, ago: ago(plan.sent.read.at) })}</span></>
          : <><Word tone="idle">{t("community.posts.status.waiting")}</Word> <span>{t("community.tester.plan.sent", { ago: ago(plan.sent.at) })}</span></>}</p>
          : <p className="deck-note">{t("community.tester.plan.say", { done, count: plan.steps.length, until: untilDate(plan.until) })}</p>}
        <div className="deck-list">
          {plan.steps.map((x, i) => <Pick key={x.id} id={`step-${x.id}`} first={i === plan.steps.findIndex((y) => !y.result)} title={`${i + 1}. ${x.title}`} sub={x.note || (x.for ? t("community.tester.plan.for", { what: x.for.label }) : undefined)}
            end={<Word tone={resultTone(x.result)}>{t(`community.tester.plan.result.${x.result ?? "none"}`)}</Word>} a={t("common.open")} onClick={() => { if (!plan.sent) setStep(x); }} />)}
          {!plan.sent && <Pick id="attach" title={t("community.tester.plan.attach")} sub={t("community.tester.plan.attachHint")} on={attach}
            end={<Word tone={attach ? "ok" : "idle"}>{attach ? t("community.deck.on") : t("community.deck.off")}</Word>} a={t("community.deck.toggle")} onClick={() => setAttach(!attach)} />}
          {!plan.sent && attach && <Pick id="attached" title={t("community.deck.whatIsSent")} a={tt("deck.shared.read")} onClick={() => setFiles(true)}
            sub={report.data ? t("community.send.check.zipHint", { size: fmtBytes(report.data.size), count: report.data.files.length }) : t("community.send.checking")} />}
        </div>
        {plan.hidden > 0 && <p className="deck-dim">{t("community.tester.plan.hidden", { count: plan.hidden })}</p>}
      </div>
    </Dialog>
  );
}
