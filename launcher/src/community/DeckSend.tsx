// "Send to Kiln support" with the gamepad (WIREFRAMES K5 to K8): pick what went wrong, pick when it happens, check and
// send. Typing is optional (Steam's keyboard); the post's title is made from the two picks. A known problem shows
// first on the check step. Sent: a QR code opens the post on the phone. The draft lives in the shared store, so it
// survives a sign-in and every error.
import { useEffect, useState } from "react";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { ProblemKind } from "../api/commands";
import { t as tt, useT } from "../i18n";
import { fmtBytes } from "../ui";
import { DeckButton, Dialog, Word } from "../deck/ui";
import { errKind, refreshCommunity, useCommunity, useCommunityUi, useSendOpen } from "./state";
import { KINDS, QrCode, kindKey, rateText, versionsLine, whenKey } from "./parts";
import { Pick } from "./DeckCommunity";
import { DeckSignIn } from "./SignIn";

const WHENS = ["start", "menu", "load", "play", "join", "other"] as const;
/** The title the two picks make: "The game crashes when I load a world". */
export const pickedTitle = (kind: ProblemKind, when: string | null | undefined) => `${tt(`community.deck.titles.${kindKey(kind)}`)} ${tt(`community.deck.when.${whenKey(when ?? "other")}`)}`;

export function DeckSendHost() {
  const open = useSendOpen((s) => s.open);
  return open ? <DeckSend /> : null;
}

type Step = "what" | "when" | "check" | "type" | "sent" | "known" | "files" | "signin";

function DeckSend() {
  const t = useT();
  const qc = useQueryClient();
  const ui = useCommunityUi();
  const s = ui.send!;
  const d = s.draft;
  const st = useCommunity();
  const acct = st.data?.account ?? null;
  const prep = useQuery({ queryKey: ["community", "prepare", s.session], queryFn: () => api.communityReportPrepare(s.target, s.from), staleTime: Infinity, gcTime: 30 * 60_000, retry: false });
  const p = prep.data;
  const [step, setStep] = useState<Step>(s.step === "sent" ? "sent" : s.step === "check" ? "check" : "what");
  const [err, setErr] = useState<{ kind: string; detail: Record<string, unknown> } | null>(null);
  const [busy, setBusy] = useState(false);
  const close = () => ui.closeSend();
  const send = async (zip = d.zip) => {
    if (!p) return;
    setBusy(true); setErr(null);
    try {
      const post = await api.communityReportSend(p.prepared, { ...d, zip, title: d.title || pickedTitle(d.kind, d.when) });
      ui.setSend({ step: "sent", sentUrl: post.url });
      void refreshCommunity(qc);
      setStep("sent");
    } catch (e) {
      const kind = errKind(e) ?? "relay";
      setErr({ kind, detail: (e as { detail?: Record<string, unknown> }).detail ?? {} });
      // too big: the zip row goes to Nobody, Send stays on
      if (kind === "too_big") ui.setDraft({ zip: "none" });
      // the prepared report is gone (over 30 minutes): a new one is made, for the player to check again
      if (kind === "not_found") void qc.resetQueries({ queryKey: ["community", "prepare", s.session] });
    } finally { setBusy(false); }
  };
  if (step === "signin") return <DeckSignIn onClose={() => setStep("check")} />;
  if (step === "what") {
    return (
      <Dialog key="what" name="send-what" title={t("community.deck.what")} onClose={close}>
        <div className="deck-list" data-testid="deck-send-what">
          {KINDS.map((k) => <Pick key={k} id={`kind-${k}`} title={t(`community.kind.${kindKey(k)}`)} on={d.kind === k} first={d.kind === k} a={t("community.deck.pick")}
            onClick={() => { ui.setDraft({ kind: k, title: "" }); setStep("when"); }} />)}
        </div>
      </Dialog>
    );
  }
  if (step === "when") {
    return (
      <Dialog key="when" name="send-when" title={t("community.deck.when.title")} onClose={() => setStep("what")} backLabel={t("common.back")}>
        <div className="deck-list" data-testid="deck-send-when">
          {WHENS.map((w, i) => <Pick key={w} id={`when-${w}`} title={pickedTitle(d.kind, w)} first={d.when ? d.when === w : i === 0} on={d.when === w}
            a={t("community.deck.pick")} onClick={() => { ui.setDraft({ when: w, title: pickedTitle(d.kind, w) }); setStep("check"); }} />)}
        </div>
      </Dialog>
    );
  }
  if (step === "type") return <TypeDialog value={d.text} onDone={(v) => { ui.setDraft({ text: v }); setStep("check"); }} />;
  if (step === "known" && p?.known) {
    return (
      <Dialog key="known" name="send-known" title={p.known.title} wide onClose={() => setStep("check")} backLabel={t("common.back")}
        buttons={<DeckButton primary onClick={() => setStep("check")}>{t("common.ok")}</DeckButton>}>
        <div data-testid="deck-known">
          <p className="deck-say">{p.known.fix}</p>
          {p.known.copy && <p className="deck-code">{p.known.copy}</p>}
          <p className="deck-note">{t("community.send.known.fixes")}</p>
        </div>
      </Dialog>
    );
  }
  if (step === "files" && p) {
    return (
      <Dialog key="files" name="send-files" title={t("community.deck.whatIsSent")} wide onClose={() => setStep("check")} backLabel={t("common.back")}
        buttons={<DeckButton primary onClick={() => setStep("check")}>{t("common.ok")}</DeckButton>}>
        <div className="deck-scroll kdc-sent-list" tabIndex={0} data-deck-a="-" data-deck-id="sent-list" data-testid="deck-what-is-sent">
          <p className="deck-label">{t("community.send.check.public")}</p>
          <p>{d.title || pickedTitle(d.kind, d.when)}. {t(`community.kind.${kindKey(d.kind)}`)}. {p.findings.map((f) => f.text).join(" ")}</p>
          <p className="deck-dim">{versionsLine(p.versions)}</p>
          <p className="deck-label">{t("community.send.check.zip")} · {fmtBytes(p.size)}</p>
          <ul>{p.files.map((f) => <li key={f.path} className="deck-mono">{f.path}</li>)}</ul>
          <p className="deck-dim">{t("community.send.check.redacted")}</p>
        </div>
      </Dialog>
    );
  }
  if (step === "sent") {
    return (
      <Dialog key="sent" name="send-sent" title={t("community.send.sent.title")} wide onClose={close} backLabel={t("common.close")}
        buttons={<DeckButton primary onClick={close}>{t("community.deck.done")}</DeckButton>}>
        <div className="kdc-qr" data-testid="deck-sent">
          <div className="kdc-qr-code">{s.sentUrl && <QrCode text={s.sentUrl} size={200} />}</div>
          <div className="kdc-qr-text">
            <p className="deck-say">{t("community.send.sent.text", { title: d.title })}</p>
            <p>{t(`community.send.sent.${d.zip === "team" ? "team" : d.zip === "public" ? "public" : "none"}`)} {t("community.send.sent.replies")}</p>
            <p className="deck-note">{t("community.deck.postQr")}</p>
          </div>
        </div>
      </Dialog>
    );
  }
  // check and send
  const zipWord = d.zip === "team" ? t("community.send.check.team") : d.zip === "public" ? t("community.send.check.everyone") : t("community.send.check.noZip");
  const nextZip = d.zip === "team" ? "public" : d.zip === "public" ? "none" : "team";
  const errWord = (k: string) => k === "offline" ? t("community.send.err.offlineTitle") : k === "not_member" ? t("community.send.err.notMemberTitle") : k === "rate_limited" ? t("community.send.err.rateTitle")
    : k === "too_big" ? t("community.send.err.bigTitle") : k === "signin" ? t("community.account.out") : k === "too_young" ? t("community.send.err.tooNewTitle")
    : k === "not_found" ? t("community.send.err.staleTitle") : t("community.send.err.relayTitle");
  const errText = (k: string, x: Record<string, unknown>) => k === "offline" ? t("community.send.err.offline") : k === "not_member" ? t("community.send.err.notMember")
    : k === "rate_limited" ? rateText(x) : k === "too_big" ? t("community.send.err.big", { size: fmtBytes(Number(x.size ?? p?.size)), limit: fmtBytes(Number(x.limit ?? 8 * 1024 * 1024)) })
    : k === "signin" ? t("community.send.check.signInHint") : k === "too_young" ? t("community.send.err.tooNew") : k === "not_found" ? t("community.send.err.stale") : t("community.send.err.relay");
  return (
    <Dialog key="check" name="send-check" title={t("community.deck.check")} wide onClose={() => setStep("when")} backLabel={t("common.back")}
      buttons={acct
        ? <DeckButton primary disabled={busy || !st.data?.online || !p} onClick={() => void send()}>{busy ? t("community.send.check.sending") : t("community.send.check.send")}</DeckButton>
        : <DeckButton primary onClick={() => setStep("signin")}>{t("community.send.check.signInToSend")}</DeckButton>}>
      <div className="deck-list" data-testid="deck-send-check">
        {p?.known && <Pick id="known" title={p.known.title} sub={t("community.send.known.fixes")} end={<Word tone="warn">{t("community.send.known.word")}</Word>} a={tt("deck.shared.read")} onClick={() => setStep("known")} />}
        <Pick id="title" title={d.title || pickedTitle(d.kind, d.when)} sub={t("community.send.check.postTitle")} a={t("community.deck.edit")} onClick={() => setStep("what")} end={<span className="deck-dim">{t("community.deck.edit")}</span>} />
        <Pick id="words" title={d.text || t("community.deck.wordsNone")} sub={t("community.send.check.words")} a={t("community.deck.type")} onClick={() => setStep("type")} end={<span className="deck-dim">{t("community.deck.type")}</span>} />
        <Pick id="zip" title={zipWord} sub={`${t("community.deck.zipRow")}${p ? `, ${fmtBytes(p.size)}` : ""}`} a={t("community.deck.toggle")} onClick={() => ui.setDraft({ zip: nextZip })} end={<span className="deck-dim">{t("community.deck.toggle")}</span>} />
        <Pick id="files" title={t("community.deck.whatIsSent")} sub={p ? p.findings.map((f) => f.text).join(" ") : t("community.send.checking")} a={tt("deck.shared.read")} onClick={() => setStep("files")} />
      </div>
      {!acct && <p className="deck-note">{t("community.send.check.signInHint")}</p>}
      {acct && !err && <p className="deck-note">{t("community.send.check.as", { name: acct.name })}. {t("community.send.check.publicHint")}</p>}
      {!err && st.data && !st.data.online && <p className="deck-line" data-testid="deck-send-offline"><Word tone="warn">{t("community.send.err.offlineTitle")}</Word> <span className="deck-dim">{t("community.send.err.offline")}</span></p>}
      {err && <p className="deck-line" data-testid="deck-send-err"><Word tone={err.kind === "relay" ? "err" : "warn"}>{errWord(err.kind)}</Word> <span className="deck-dim">{errText(err.kind, err.detail)}</span></p>}
    </Dialog>
  );
}

/** Words for the post: the field takes the focus and Steam is asked for its keyboard. */
export function TypeDialog({ value, onDone, max = 300 }: { value: string; onDone: (v: string) => void; max?: number }) {
  const t = useT();
  const [v, setV] = useState(value);
  useEffect(() => { api.communitySteamKeyboard().catch(() => {}); }, []);
  return (
    <Dialog key="type" name="send-type" title={t("community.deck.typeTitle")} wide onClose={() => onDone(value)} backLabel={t("common.cancel")}
      buttons={<DeckButton primary onClick={() => onDone(v.trim())}>{t("community.deck.done")}</DeckButton>}>
      <input className="kdc-input" data-deck-autofocus="" data-testid="deck-type" aria-label={t("community.deck.typeTitle")} value={v} maxLength={max} placeholder={t("community.send.morePh")} onChange={(e) => setV(e.target.value)} />
      <p className="deck-note">{t("community.deck.typeHint")}</p>
    </Dialog>
  );
}
