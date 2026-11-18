// Signing in with Discord: the desktop dialog and the Deck's QR pairing (step 4 of the Discord work).
import { Row } from "../deck/ui";
import { useT } from "../i18n";
import { useCommunity } from "./state";

/** The Deck's account row: who is signed in, or "Sign in with Discord". */
export function DeckSignInRow() {
  const t = useT();
  const s = useCommunity().data;
  const acct = s?.account ?? null;
  if (acct) {
    return <Row id="account" title={acct.name} sub={acct.member ? t("community.deck.accountSub") : t("community.deck.accountSubOut")}
      icon={<span className="kc-avatar big">{acct.name.slice(0, 1).toUpperCase()}</span>} />;
  }
  return <Row id="signin" title={t("community.account.signIn")} sub={t("community.deck.signInSub")} disabled={!s?.online} />;
}
