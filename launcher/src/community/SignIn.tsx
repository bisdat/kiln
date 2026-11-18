// Signing in with Discord, always through frostdev.io (WIREFRAMES D5, K3). Desktop: the system browser and back; the
// dialog waits and moves on by itself. Deck: a QR code and a short code for a phone, with the time left and a new code
// on X. Every ending keeps the dialog with Try again (desktop) or New code (Deck); a sign-in started from the send
// dialog goes back to it with the draft. The launcher holds only the Kiln session key afterwards.
import { useEffect, useRef, useState } from "react";
import { useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { CommunityAccount } from "../api/commands";
import { t as tt, useT } from "../i18n";
import { Banner, Button, Cones, Modal, toast } from "../ui";
import { DeckButton, Dialog, Row, Word } from "../deck/ui";
import { PRIVACY_URL, errKind, refreshCommunity, useCommunity, useCommunityUi, useSendOpen } from "./state";
import { QrCode } from "./parts";
import { DeckAccountDialog } from "./Privacy";

const open = (url: string) => { api.appOpenUrl(url).catch((e) => toast.error(e)); };
const tabTexts = (): api.SignInTexts => ({ done: tt("community.signIn.tabDone"), failed: tt("community.signIn.tabFailed") });
/** How often the dialog asks how the attempt is going (the backend asks the relay at its own pace). */
const POLL_MS = 700;

// ---------------------------------------------------------------- desktop

type Phase = "start" | "waiting" | "canceled" | "expired" | "denied" | "offline" | "relay";

export function SignInDialog({ resumeSend }: { resumeSend: boolean }) {
  const t = useT();
  const qc = useQueryClient();
  const ui = useCommunityUi();
  const [join, setJoin] = useState(true);
  const [phase, setPhase] = useState<Phase>("start");
  const [pend, setPend] = useState<{ id: string; url: string } | null>(null);
  const [busy, setBusy] = useState(false);
  // a sign-in started from the send dialog goes back to it, with the draft, however it ends
  const close = () => { if (pend) api.communitySignInCancel(pend.id).catch(() => {}); ui.closeSignIn(); if (resumeSend) useSendOpen.setState({ open: true }); };
  const start = async () => {
    setBusy(true);
    try {
      const s = await api.communitySignInStart("browser", join, tabTexts());
      setPend({ id: s.id, url: s.url });
      setPhase("waiting");
      open(s.url);
    } catch (e) { setPhase(errKind(e) === "offline" ? "offline" : "relay"); } finally { setBusy(false); }
  };
  useEffect(() => {
    if (phase !== "waiting" || !pend) return;
    let live = true;
    const timer = setInterval(async () => {
      let r: api.SignInState | null;
      try { r = await api.communitySignInPoll(pend.id); } catch (e) {
        r = null;
        // offline ends the attempt here and in the backend (its listener on 127.0.0.1 closes); Try again starts anew
        if (errKind(e) === "offline") { clearInterval(timer); api.communitySignInCancel(pend.id).catch(() => {}); if (live) { setPend(null); setPhase("offline"); } }
      }
      if (!live || !r || r.state === "waiting") return;
      clearInterval(timer);
      if (r.state === "approved") {
        await refreshCommunity(qc);
        toast.ok(t("community.signIn.done", { name: r.account.name }), t("community.signIn.doneDetail"));
        ui.closeSignIn();
        // the draft is still in the send dialog, at its check step
        if (resumeSend) { useCommunityUi.getState().setSend({ step: "check" }); useSendOpen.setState({ open: true }); }
        return;
      }
      setPend(null);
      setPhase(r.state);
    }, POLL_MS);
    return () => { live = false; clearInterval(timer); };
  }, [phase, pend]);
  if (phase === "waiting") {
    return (
      <Modal open onClose={close} title={t("community.signIn.waitingTitle")} width={520}
        footer={<><button type="button" className="k-link" style={{ marginRight: "auto" }} onClick={() => pend && open(pend.url)}>{t("community.signIn.again")}</button><Button onClick={close}>{t("common.cancel")}</Button></>}>
        <div className="k-stack" style={{ gap: 12 }} data-testid="kc-signin-wait">
          <div className="k-row" style={{ gap: 12 }}><Cones step={0} count={3} /><span className="k-sc kc-waiting">{t("community.signIn.waitingWord")}</span></div>
          <p className="kc-p">{t("community.signIn.waiting")}</p>
        </div>
      </Modal>
    );
  }
  const errText = phase === "canceled" ? t("community.signIn.canceled") : phase === "expired" ? t("community.signIn.expired") : phase === "denied" ? t("community.signIn.denied")
    : phase === "offline" ? t("community.signIn.offline") : phase === "relay" ? t("community.signIn.relay") : null;
  return (
    <Modal open onClose={close} title={t("community.signIn.title")} width={540}
      footer={<><Button onClick={close}>{t("common.cancel")}</Button><Button variant="primary" loading={busy} onClick={start} data-testid="kc-signin-open">{errText ? t("community.signIn.retry") : t("community.signIn.open")}</Button></>}>
      <div className="k-stack" style={{ gap: 12 }} data-testid="kc-signin-dialog">
        {errText && <div data-testid="kc-signin-error"><Banner tone={phase === "canceled" || phase === "denied" ? "info" : "warn"}>{errText}</Banner></div>}
        <p className="kc-p">{t("community.signIn.text")}</p>
        <p className="kc-p k-muted">{t("community.signIn.gets")}</p>
        <label className="kc-tick"><input type="checkbox" className="k-check" checked={join} onChange={(e) => setJoin(e.target.checked)} data-testid="kc-also-join" />
          <span><span>{t("community.account.alsoJoin")}</span><br /><span className="k-hint">{t("community.signIn.joinHint")}</span></span></label>
        <div className="kc-links-row">
          <button type="button" className="k-link" onClick={() => ui.setPrivacy(true)}>{t("community.account.whatIsSent")}</button>
          <button type="button" className="k-link" data-testid="kc-privacy-page" onClick={() => open(PRIVACY_URL)}>{t("community.privacy.page")}</button>
        </div>
      </div>
    </Modal>
  );
}

// ---------------------------------------------------------------- Deck: QR pairing

/** The Deck's account row: who is signed in (A: the account and what is sent), or "Sign in with Discord". */
export function DeckSignInRow() {
  const t = useT();
  const s = useCommunity().data;
  const acct = s?.account ?? null;
  const [open, setOpen] = useState(false);
  if (acct) {
    return (
      <>
        <Row id="account" title={acct.name} sub={acct.member ? t("community.deck.accountSub") : t("community.deck.accountSubOut")} a={t("common.open")} onClick={() => setOpen(true)}
          icon={<span className="kc-avatar big">{acct.name.slice(0, 1).toUpperCase()}</span>} />
        {open && <DeckAccountDialog onClose={() => setOpen(false)} />}
      </>
    );
  }
  return (
    <>
      <Row id="signin" title={t("community.account.signIn")} sub={t("community.deck.signInSub")} a={t("common.open")} onClick={() => setOpen(true)} disabled={s ? !s.online : false} />
      {open && <DeckSignIn onClose={() => setOpen(false)} />}
    </>
  );
}

type QrState = "waiting" | "expired" | "denied" | "offline" | "relay";

export function DeckSignIn({ onClose, onDone }: { onClose: () => void; onDone?: (a: CommunityAccount) => void }) {
  const t = useT();
  const qc = useQueryClient();
  const [pair, setPair] = useState<api.SignInStart | null>(null);
  const [state, setState] = useState<QrState>("waiting");
  const [now, setNow] = useState(Date.now());
  const fresh = () => {
    setState("waiting");
    setPair(null);
    // join: frostdev.io's phone page then offers "Also join the Kiln Discord" (ticked) to a player who isn't in it; the
    // choice is made there, on the Approve page (PLAN 3.2, docs/relay-api.md)
    api.communitySignInStart("qr", true).then(setPair).catch((e) => setState(errKind(e) === "offline" ? "offline" : "relay"));
  };
  // one pairing per opening (a development double mount must not start a second one)
  const begun = useRef(false);
  useEffect(() => { if (!begun.current) { begun.current = true; fresh(); } }, []);
  useEffect(() => {
    if (!pair || state !== "waiting") return;
    let live = true;
    const timer = setInterval(async () => {
      setNow(Date.now());
      if (Date.now() > new Date(pair.expiresAt).getTime()) { setState("expired"); return; }
      const r = await api.communitySignInPoll(pair.id).catch((e) => { if (errKind(e) === "offline" && live) setState("offline"); return null; });
      if (!live || !r || r.state === "waiting") return;
      if (r.state === "approved") {
        await refreshCommunity(qc);
        toast.ok(t("community.signIn.done", { name: r.account.name }));
        onClose();
        onDone?.(r.account);
        return;
      }
      setState(r.state === "denied" ? "denied" : "expired");
    }, POLL_MS);
    return () => { live = false; clearInterval(timer); };
  }, [pair, state]);
  const left = pair ? Math.max(0, Math.round((new Date(pair.expiresAt).getTime() - now) / 1000)) : 0;
  const clock = `${Math.floor(left / 60)}:${String(left % 60).padStart(2, "0")}`;
  const cancel = () => { if (pair) api.communitySignInCancel(pair.id).catch(() => {}); };
  const close = () => { cancel(); onClose(); };
  const again = () => { cancel(); fresh(); };
  return (
    <Dialog name="community-signin" title={t("community.deck.qr.title")} wide onClose={close}
      actions={[{ button: "x", label: t("community.deck.qr.newCode"), run: again }]}
      buttons={state !== "waiting" ? <DeckButton primary onClick={again}>{t("community.deck.qr.newCode")}</DeckButton> : undefined}>
      <div className="kdc-qr" data-testid="deck-qr" data-state={state}>
        <div className="kdc-qr-code">
          {pair && state === "waiting" ? <QrCode text={pair.url} size={232} title={pair.url} /> : <div className="kdc-qr-gone" />}
          <div className="kdc-code" data-testid="deck-qr-code">{pair?.code ?? "····-····"}</div>
        </div>
        <div className="kdc-qr-text">
          <ol className="kdc-steps">
            <li>{t("community.deck.qr.step1")}</li>
            <li>{t("community.deck.qr.step2")}</li>
            <li>{t("community.deck.qr.step3", { code: pair?.code ?? "" })}</li>
          </ol>
          <p className="deck-note">{t("community.deck.qr.noCamera")}</p>
          <p className="deck-note">{t("community.deck.qr.privacy")}</p>
          <p className="deck-line" role="status">
            {state === "waiting" ? <><Word tone="ember">{t("community.signIn.waitingWord")}</Word> <span className="deck-dim">{t("community.deck.qr.waiting", { left: clock })}</span></>
              : state === "offline" ? <Word tone="warn">{t("community.signIn.offline")}</Word>
              : state === "relay" ? <Word tone="warn">{t("community.signIn.relay")}</Word>
              : state === "denied" ? <Word tone="warn">{t("community.deck.qr.denied")}</Word> : <Word tone="warn">{t("community.deck.qr.expired")}</Word>}
          </p>
        </div>
      </div>
    </Dialog>
  );
}
