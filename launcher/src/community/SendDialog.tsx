// "Send to Kiln support" on the desktop (WIREFRAMES D6 to D9): write (one line, the kind, more words; a known
// problem's fix first) -> check what is sent (the public post as it will look, the zip with its files and who sees it,
// what is never in it) -> sent (Open in Discord). The report is prepared once when the dialog opens and sent as
// prepared. Not signed in: Sign in keeps the draft and comes back to the check step. Every error stays in the dialog,
// with the draft.
import { useEffect, useRef, useState } from "react";
import { useNavigate } from "react-router-dom";
import { keepPreviousData, useQuery, useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { KnownAction, ReportDraft, ReportPreview } from "../api/commands";
import { useT } from "../i18n";
import { Banner, Button, Cones, Field, KeyValue, Modal, Skeleton, StatusPill, TextArea, TextInput, fmtBytes, toast } from "../ui";
import { INVITE, errKind, openInDiscord, refreshCommunity, useCommunity, useCommunityUi, useSendOpen } from "./state";
import { KINDS, discordWords, kindKey, rateText, versionsLine } from "./parts";
import { ReportContentsView, SummaryText, replacedInAll, replacedText, totalText } from "../components/ReportContents";

const MAX = 90;
const WORDS_MAX = 1500;
const open = (url: string) => { api.appOpenUrl(url).catch((e) => toast.error(e)); };

/** Where a known problem's own button goes. */
export const KNOWN_ROUTE: Record<KnownAction, string> = { mods: "/", runtime: "/runtime", logs: "/logs", settings: "/settings" };

/** The public post's tags for the kind the player picked: the kind's, then the system's. */
export const postTags = (p: ReportPreview, kind: ReportDraft["kind"]) => [...new Set([p.kindTags?.[kind], ...p.tags].filter((x): x is string => !!x))].slice(0, 5);

type SendErr = { kind: string; detail: Record<string, unknown> } | null;

export function SendDialog() {
  const t = useT();
  const qc = useQueryClient();
  const nav = useNavigate();
  const ui = useCommunityUi();
  const s = ui.send!;
  const d = s.draft;
  const st = useCommunity();
  const acct = st.data?.account ?? null;
  // other mods' logs the player left out ("What's in this report"): each change prepares the report again
  const [exclude, setExclude] = useState<string[]>([]);
  const prep = useQuery({ queryKey: ["community", "prepare", s.session, exclude], queryFn: () => api.communityReportPrepare(s.target, s.from, exclude), staleTime: Infinity, gcTime: 30 * 60_000,
    retry: false, placeholderData: keepPreviousData });
  const p = prep.data;
  // Send sends the report prepared for the boxes as they are now, never the one before a change
  const stale = prep.isPlaceholderData;
  const [summary, setSummary] = useState(false);
  const [err, setErr] = useState<SendErr>(null);
  const [busy, setBusy] = useState(false);
  const [files, setFiles] = useState(false);
  const [tried, setTried] = useState(false);
  const lineRef = useRef<HTMLInputElement>(null);
  // an error shows where the eye is: the dialog's body scrolls to it (the check step is taller than the window)
  const errRef = useRef<HTMLDivElement>(null);
  useEffect(() => { if (err || (st.data && !st.data.online)) setTimeout(() => errRef.current?.scrollIntoView({ block: "nearest" }), 30); }, [err, st.data?.online, s.step]);
  useEffect(() => { if (s.step === "write") setTimeout(() => lineRef.current?.focus(), 60); }, [s.step]);
  const draft = (x: Partial<ReportDraft>) => ui.setDraft(x);
  const close = () => ui.closeSend();
  const send = async (zip = d.zip) => {
    if (!p) return;
    setBusy(true); setErr(null);
    try {
      const post = await api.communityReportSend(p.prepared, { ...d, zip });
      ui.setDraft({ zip });
      ui.setSend({ step: "sent", sentUrl: post.url });
      void refreshCommunity(qc);
    } catch (e) {
      const kind = errKind(e) ?? "relay";
      setErr({ kind, detail: ((e as { detail?: Record<string, unknown> }).detail ?? {}) });
      // the prepared report is gone (over 30 minutes): a new one is made, for the player to check again
      if (kind === "not_found") void qc.resetQueries({ queryKey: ["community", "prepare", s.session] });
      void refreshCommunity(qc);
    } finally { setBusy(false); }
  };
  const save = () => { close(); nav("/logs"); };

  if (s.step === "sent") {
    return (
      <Modal open onClose={close} title={t("community.send.sent.title")} width={560}
        footer={<><Button onClick={close}>{t("common.close")}</Button><Button variant="primary" onClick={() => { if (s.sentUrl) openInDiscord(s.sentUrl); close(); }}>{t("community.send.sent.open")}</Button></>}>
        <div className="k-stack" style={{ gap: 10 }} data-testid="kc-sent">
          <p className="kc-p">{t("community.send.sent.text", { title: d.title })}</p>
          <p className="kc-p">{t(`community.send.sent.${d.zip === "team" ? "team" : d.zip === "public" ? "public" : "none"}`)}</p>
          <p className="kc-p k-muted">{t("community.send.sent.replies")}</p>
        </div>
      </Modal>
    );
  }

  if (s.step === "write") {
    // the relay takes a line of 3 characters or more
    const short = d.title.trim().length < 3;
    return (
      <Modal open onClose={close} title={t("community.send.title")} width={640}
        footer={<>
          <Button onClick={close}>{t("common.cancel")}</Button>
          <Button variant="primary" data-testid="kc-next" onClick={() => { setTried(true); if (!short) ui.setSend({ step: "check" }); }}>{t("community.send.next")}</Button>
        </>}>
        <div className="k-stack kc-send" style={{ gap: 16 }} data-testid="kc-write">
          {prep.isLoading && <div className="k-row k-muted"><Cones step={0} count={3} /> {t("community.send.checking")}</div>}
          {p?.known && <Known known={p.known} onAction={(to) => { close(); nav(to); }} />}
          <Field label={t("community.send.line")} htmlFor="kc-line" error={tried && short ? t("community.send.lineNeeded") : undefined}
            hint={t("community.send.lineCount", { count: d.title.length, max: MAX })}>
            <TextInput id="kc-line" ref={lineRef} value={d.title} maxLength={MAX} invalid={tried && short} placeholder={t(`community.send.linePh.${kindKey(d.kind)}`)} onChange={(e) => draft({ title: e.target.value })} />
          </Field>
          <Field label={t("community.send.kind")}>
            <div className="kc-kinds" role="radiogroup" aria-label={t("community.send.kind")}>
              {KINDS.map((k) => (
                <button key={k} type="button" role="radio" aria-checked={d.kind === k} className={"kc-kind" + (d.kind === k ? " on" : "")} onClick={() => draft({ kind: k })}>{t(`community.kind.${kindKey(k)}`)}</button>
              ))}
            </div>
          </Field>
          <Field label={t("community.send.more")} htmlFor="kc-more">
            <TextArea id="kc-more" className="kc-prose" rows={3} maxLength={WORDS_MAX} value={d.text} placeholder={t("community.send.morePh")} onChange={(e) => draft({ text: e.target.value })} />
          </Field>
        </div>
      </Modal>
    );
  }

  // check what is sent
  const zipLabel = p ? t("community.send.check.zipHint", { size: fmtBytes(p.size), count: p.files.length }) : "";
  const canSend = !!acct;
  return (
    <Modal open onClose={close} title={t("community.send.check.title")} width={680} locked={busy}
      footer={<>
        <Button onClick={() => { setErr(null); ui.setSend({ step: "write" }); }} disabled={busy}>{t("common.back")}</Button>
        {canSend
          ? <Button variant="primary" loading={busy} disabled={!st.data?.online || !p || stale} data-testid="kc-send-now" onClick={() => send()}>{t("community.send.check.send")}</Button>
          : <Button variant="primary" data-testid="kc-send-signin" onClick={() => { useSendOpen.setState({ open: false }); ui.openSignIn(true); }}>{t("community.send.check.signInToSend")}</Button>}
      </>}>
      {prep.error ? <Banner tone="err" title={t("community.send.err.prepareTitle")}>{(prep.error as { message?: string }).message ?? ""}</Banner> : !p ? <Skeleton lines={5} /> : (
        <div className="k-stack kc-check" style={{ gap: 16 }} data-testid="kc-check">
          <section className="kc-block">
            <div className="kc-block-head"><span className="k-label">{t("community.send.check.public")}</span></div>
            <div className="kc-postmock" data-testid="kc-postmock">
              <div className="kc-postmock-title">{d.title}</div>
              <div className="kc-postmock-tags">{postTags(p, d.kind).map((x) => <span key={x}>{x}</span>)}</div>
              {d.text && <p className="kc-postmock-text">{d.text}</p>}
              <KeyValue rows={[
                [t("community.send.check.kind"), t(`community.kind.${kindKey(d.kind)}`)],
                [t("community.send.check.found"), <ul className="kc-found">{p.findings.map((f) => <li key={f.id}><span className={`k-sc k-tone-${f.level === "problem" ? "err" : f.level === "warning" ? "warn" : "info"}`}>{t(`community.send.check.level.${f.level}`)}</span> {f.text}</li>)}</ul>],
                [t("community.send.check.versions"), <span className="kc-versions">{versionsLine(p.versions)}</span>],
              ]} />
            </div>
            <span className="k-hint">{acct ? `${t("community.send.check.as", { name: acct.name })}. ` : ""}{t("community.send.check.publicHint")}</span>
          </section>
          <section className="kc-block">
            <div className="kc-block-head">
              <span className="k-label">{t("community.send.check.zip")}</span>
              <span className="k-hint">{zipLabel}</span>
              <button type="button" className="k-link kc-files-toggle" data-testid="kc-files-toggle" onClick={() => setFiles(!files)}>{files ? t("community.send.check.hideFiles") : t("reportContents.title")}</button>
            </div>
            {p.contents && <span className="k-hint" data-testid="kc-replaced">{[totalText(p.contents), replacedText(replacedInAll(p.contents))].filter(Boolean).join(". ")}</span>}
            {files && p.contents && (
              <div className="k-stack kc-files-contents" style={{ gap: 10 }} data-testid="kc-files">
                <div className="krc-total">
                  <span className="k-label">{t("reportContents.title")}</span>
                  <Button size="s" variant="ghost" data-testid="krc-view" onClick={() => setSummary(!summary)}>{summary ? t("reportContents.hideSummary") : t("reportContents.view")}</Button>
                </div>
                {summary && <SummaryText text={p.contents.summary} />}
                <ReportContentsView contents={p.contents} exclude={exclude} onExclude={setExclude} disabled={busy} />
              </div>
            )}
            <div className="kc-who" role="radiogroup" aria-label={t("community.send.check.who")}>
              <div className="k-label">{t("community.send.check.who")}</div>
              {(["team", "public", "none"] as const).map((z) => (
                <label key={z} className={"kc-radio" + (d.zip === z ? " on" : "")}>
                  <input type="radio" name="kc-zip" value={z} checked={d.zip === z} onChange={() => draft({ zip: z })} />
                  <span><span className="kc-radio-title">{t(`community.send.check.${z === "team" ? "team" : z === "public" ? "everyone" : "noZip"}`)}</span>
                    <span className="k-hint">{t(`community.send.check.${z === "team" ? "teamHint" : z === "public" ? "everyoneHint" : "noZipHint"}`)}</span></span>
                </label>
              ))}
            </div>
            <span className="k-hint">{t("community.send.check.redacted")}</span>
          </section>
          <div ref={errRef} className="k-stack" style={{ gap: 8 }} data-testid="kc-send-state">
            {!canSend && <Banner tone="info" title={t("community.account.out")}>{t("community.send.check.signInHint")}</Banner>}
            {!err && st.data && !st.data.online && <Banner tone="warn" title={t("community.send.err.offlineTitle")} actions={<Button size="s" onClick={save}>{t("community.send.err.saveReport")}</Button>}>{t("community.send.err.offline")}</Banner>}
            {err && <SendError err={err} size={p.size} retry={() => send()} sendWithout={() => send("none")} save={save} recheck={async () => {
              const a = await api.communityCheckMember().catch(() => null);
              await refreshCommunity(qc);
              if (a?.member) setErr(null); else toast.info(t("community.send.err.stillNot"));
            }} />}
          </div>
        </div>
      )}
    </Modal>
  );
}

function Known({ known, onAction }: { known: NonNullable<ReportPreview["known"]>; onAction: (to: string) => void }) {
  const t = useT();
  return (
    <div className="kc-known" data-testid="kc-known">
      <div className="kc-known-head"><StatusPill tone="warn">{t("community.send.known.word")}</StatusPill> <b>{known.title}</b></div>
      <p className="kc-p">{known.fix}</p>
      {known.copy && (
        <div className="kc-copy">
          <code className="k-mono">{known.copy}</code>
          <Button size="s" onClick={() => navigator.clipboard?.writeText(known.copy!).then(() => toast.ok(t("community.send.known.copied"))).catch(() => {})}>{t("community.send.known.copy")}</Button>
        </div>
      )}
      {known.action && <div><Button size="s" onClick={() => onAction(KNOWN_ROUTE[known.action!])}>{known.label || t(`community.send.known.action.${known.action}`)}</Button></div>}
      <span className="k-hint">{t("community.send.known.fixes")}</span>
    </div>
  );
}

function SendError({ err, size, retry, sendWithout, save, recheck }: { err: NonNullable<SendErr>; size: number; retry: () => void; sendWithout: () => void; save: () => void; recheck: () => void }) {
  const t = useT();
  const d = err.detail;
  switch (err.kind) {
    case "offline":
      return <Banner tone="warn" title={t("community.send.err.offlineTitle")} actions={<Button size="s" onClick={save}>{t("community.send.err.saveReport")}</Button>}>{t("community.send.err.offline")}</Banner>;
    case "signin":
      return <Banner tone="info" title={t("community.account.out")}>{t("community.send.check.signInHint")}</Banner>;
    case "not_member":
      return <Banner tone="info" title={t("community.send.err.notMemberTitle")} actions={<>
        <Button size="s" onClick={() => open(INVITE)}>{t("community.join")}</Button>
        <button type="button" className="k-link" onClick={recheck}>{t("community.send.err.joined")}</button>
      </>}>{t("community.send.err.notMember")}</Banner>;
    case "rate_limited":
      return <Banner tone="warn" title={t("community.send.err.rateTitle")} actions={d.last ? <Button size="s" onClick={() => openInDiscord(String(d.last))}>{t("community.send.err.openLast")}</Button> : undefined}>
        {rateText(d)}</Banner>;
    case "too_big":
      return <Banner tone="warn" title={t("community.send.err.bigTitle")} actions={<>
        <Button size="s" onClick={sendWithout}>{t("community.send.err.sendWithout")}</Button>
        <button type="button" className="k-link" onClick={save}>{t("community.send.err.saveReport")}</button>
      </>}>{t("community.send.err.big", { size: fmtBytes(Number(d.size ?? size)), limit: fmtBytes(Number(d.limit ?? 8 * 1024 * 1024)) })}</Banner>;
    case "too_young":
      return <Banner tone="warn" title={t("community.send.err.tooNewTitle")} actions={<Button size="s" onClick={save}>{t("community.send.err.saveReport")}</Button>}>{t("community.send.err.tooNew")}</Banner>;
    case "not_found":
      return <Banner tone="info" title={t("community.send.err.staleTitle")}>{t("community.send.err.stale")}</Banner>;
    case "discord": {
      // Discord refused the post, or its rate limit holds it: frostdev.io answered, so not "can't be reached"
      const w = discordWords(d, "report");
      return <Banner tone="warn" title={w.title} actions={<>
        <Button size="s" onClick={retry}>{t("common.retry")}</Button>
        <button type="button" className="k-link" onClick={save}>{t("community.send.err.saveReport")}</button>
      </>}>{w.text}</Banner>;
    }
    default:
      return <Banner tone="err" title={t("community.send.err.relayTitle")} actions={<Button size="s" onClick={retry}>{t("common.retry")}</Button>}>{t("community.send.err.relay")}</Banner>;
  }
}
