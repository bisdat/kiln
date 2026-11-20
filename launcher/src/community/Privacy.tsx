// "What is sent and kept" (WIREFRAMES D14, K11): one screen for everything the Discord features send and what
// frostdev.io keeps, how to undo each, and the links to the privacy page and the terms. Its words say what
// https://frostdev.io/kiln/privacy says, in short. On the Deck it is also where the player signs out.
import { useState } from "react";
import { useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import { useT } from "../i18n";
import { Button, Modal, toast } from "../ui";
import { DeckButton, Dialog, HoldButton, Row } from "../deck/ui";
import { PRIVACY_URL, TERMS_URL, refreshCommunity, useCommunity, useCommunityUi } from "./state";
import { QrDialog } from "./parts";

const open = (url: string) => { api.appOpenUrl(url).catch((e) => toast.error(e)); };

/** The sections in order, each with its paragraphs (en.json community.privacy.*). */
export const PRIVACY_SECTIONS: [string, string[]][] = [
  ["signInTitle", ["signIn", "join"]], ["reportTitle", ["reportPublic", "reportTeam", "ai"]], ["presenceTitle", ["presence"]],
  ["testerTitle", ["tester"]], ["keptTitle", ["kept"]], ["undoTitle", ["undo"]],
];

export function PrivacyDialog() {
  const t = useT();
  const ui = useCommunityUi();
  return (
    <Modal open onClose={() => ui.setPrivacy(false)} title={t("community.privacy.title")} width={640}
      footer={<>
        <button type="button" className="k-link" style={{ marginRight: "auto" }} data-testid="kc-privacy-page" onClick={() => open(PRIVACY_URL)}>{t("community.privacy.page")}</button>
        <button type="button" className="k-link" style={{ marginRight: 12 }} data-testid="kc-terms" onClick={() => open(TERMS_URL)}>{t("community.privacy.terms")}</button>
        <Button variant="primary" onClick={() => ui.setPrivacy(false)}>{t("common.close")}</Button>
      </>}>
      <div className="k-stack kc-privacy" style={{ gap: 14 }} data-testid="kc-privacy">
        <p className="kc-p"><b>{t("community.privacy.never")}</b></p>
        {PRIVACY_SECTIONS.map(([h, ps]) => (
          <section key={h}>
            <div className="k-label">{t(`community.privacy.${h}`)}</div>
            {ps.map((x) => <p key={x} className="kc-p">{t(`community.privacy.${x}`)}</p>)}
          </section>
        ))}
      </div>
    </Modal>
  );
}

/** The Deck tab's row. */
export function DeckPrivacyRow() {
  const t = useT();
  const [o, setO] = useState(false);
  return (
    <>
      <Row id="privacy" title={t("community.account.whatIsSent")} sub={t("community.deck.privacySub")} a={t("common.open")} onClick={() => setO(true)} />
      {o && <DeckAccountDialog onClose={() => setO(false)} />}
    </>
  );
}

/** What is sent and kept, with Sign out and Hold to sign out and forget me when signed in; Y shows the privacy page
 *  as a QR code for the phone. */
export function DeckAccountDialog({ onClose }: { onClose: () => void }) {
  const t = useT();
  const qc = useQueryClient();
  const st = useCommunity().data;
  const acct = st?.account ?? null;
  // forgetting must reach frostdev.io: not while offline or with frostdev.io not answering
  const net = st ? st.online && st.relay === "ok" : true;
  const [page, setPage] = useState(false);
  const out = (forget: boolean) => api.communitySignOut(forget).then(() => { toast.ok(t("community.account.signedOut")); })
    .catch((e) => (forget ? toast.error(t("community.privacy.forgetFailed"), t("community.privacy.forgetFailedTitle")) : toast.error(e)))
    .finally(() => { void refreshCommunity(qc); onClose(); });
  if (page) return <QrDialog title={t("community.privacy.page")} text={t("community.deck.privacyQr")} url={PRIVACY_URL} onClose={() => setPage(false)} />;
  return (
    <Dialog name="community-privacy" title={t("community.privacy.title")} wide onClose={onClose} backLabel={t("common.close")}
      actions={[{ button: "y", label: t("community.privacy.page"), run: () => setPage(true) }]}
      buttons={acct
        ? <><DeckButton id="signout" onClick={() => void out(false)}>{t("community.account.signOut")}</DeckButton><HoldButton id="forget" disabled={!net} onConfirm={() => void out(true)}>{t("community.account.forget")}</HoldButton></>
        : <DeckButton primary onClick={onClose}>{t("common.ok")}</DeckButton>}>
      <div className="deck-scroll kdc-sent-list" tabIndex={0} data-deck-a="-" data-deck-id="privacy-text" data-testid="deck-privacy">
        <p><b>{t("community.privacy.never")}</b></p>
        {PRIVACY_SECTIONS.map(([h, ps]) => <div key={h}><p className="deck-label">{t(`community.privacy.${h}`)}</p>{ps.map((x) => <p key={x}>{t(`community.privacy.${x}`)}</p>)}</div>)}
      </div>
    </Dialog>
  );
}
