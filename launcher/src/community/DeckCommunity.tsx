// The Deck's Community tab (WIREFRAMES K2), all with the gamepad: Support on the left (Send to Kiln support, your posts),
// Discord on the right (sign in or the account, Join with a QR code, Rich Presence, testers, what is sent). A opens a
// row; links go to the phone as QR codes.
import { useEffect, useState, type ReactNode } from "react";
import { useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { SupportPost } from "../api/commands";
import { useGame } from "../api/hooks";
import { useT } from "../i18n";
import { toast } from "../ui";
import { DeckButton, Dialog, HoldButton, Row, Screen, Word } from "../deck/ui";
import { INVITE, ago, openSendReport, refreshCommunity, sayLine, useCommunity, usePosts } from "./state";
import { QrCode, statusWord } from "./parts";
import { DeckPresenceRow } from "./Presence";
import { DeckTesterRow } from "./Testers";
import { DeckSignInRow } from "./SignIn";
import { DeckPrivacyRow } from "./Privacy";
import "./community.css";
import "./deck-community.css";

/** A row that can take the dialog's first focus (the preselected pick). */
export function Pick({ title, sub, end, on, onClick, id, first, a }: { title: string; sub?: string; end?: ReactNode; on?: boolean; onClick: () => void; id: string; first?: boolean; a?: string }) {
  return (
    <button type="button" className={"deck-row" + (on ? " kdc-on" : "")} onClick={onClick} data-deck-id={id} data-deck-a={a} data-deck-autofocus={first ? "" : undefined}>
      <span className="deck-row-main"><span className="deck-row-title">{title}</span>{sub && <span className="deck-row-sub">{sub}</span>}</span>
      {end && <span className="deck-row-end">{end}</span>}
    </button>
  );
}

export default function DeckCommunity() {
  const t = useT();
  const st = useCommunity();
  const s = st.data;
  const acct = s?.account ?? null;
  const posts = usePosts(!!acct);
  const { game } = useGame();
  const [join, setJoin] = useState(false);
  const [post, setPost] = useState<SupportPost | null>(null);
  useEffect(() => { if (acct) api.communityCheckMember().catch(() => {}); }, [!!acct]);
  return (
    <Screen name="community" title={t("community.title")} say={sayLine(s, true)} className="kdc">
      <div className="kdc-cols deck-scroll">
        <section className="kdc-col" aria-label={t("community.deck.support")}>
          <div className="deck-label kdc-head">{t("community.deck.support")}</div>
          <div className="deck-row-buttons"><DeckButton id="send" primary onClick={() => openSendReport("deck", game ? { game: game.id } : null)}>{t("community.send.title")}</DeckButton></div>
          <p className="deck-note">{t("community.deck.sendSub")}.</p>
          <div className="deck-label kdc-head">{t("community.posts.title")}</div>
          {!acct ? <p className="deck-note">{t("community.posts.emptyOut")}</p>
            : !(posts.data ?? []).length ? <p className="deck-note">{posts.isLoading ? "" : t("community.posts.empty")}</p>
            : (
              <div className="deck-list">
                {posts.data!.map((p) => {
                  const w = statusWord(p);
                  return <Row key={p.id} id={`post-${p.id}`} title={p.title} a={t("common.open")}
                    sub={p.lastReply ? t("community.posts.lastReply", { name: p.lastReply.name, ago: ago(p.lastReply.at) }) : t("community.posts.noReply", { ago: ago(p.created) })}
                    end={<Word tone={w.tone}>{t(`community.posts.status.${w.key}`)}</Word>} onClick={() => setPost(p)} />;
                })}
              </div>
            )}
        </section>
        <section className="kdc-col" aria-label={t("community.deck.discord")}>
          <div className="deck-label kdc-head">{t("community.deck.discord")}</div>
          <div className="deck-list">
            <DeckSignInRow />
            <Row id="join" title={acct?.member ? t("community.openDiscord") : t("community.join")} sub={t("community.deck.joinSub")} a={t("common.open")} onClick={() => setJoin(true)} />
            <DeckPresenceRow />
            <DeckTesterRow />
            <DeckPrivacyRow />
          </div>
        </section>
      </div>
      {join && <QrDialog title={t("community.deck.joinQr.title")} text={t("community.deck.joinQr.text")} url={INVITE} onClose={() => setJoin(false)} />}
      {post && <DeckPost post={post} onClose={() => setPost(null)} />}
    </Screen>
  );
}

/** A link for the phone: its QR code and the address as text. */
export function QrDialog({ title, text, url, onClose }: { title: string; text: string; url: string; onClose: () => void }) {
  const t = useT();
  return (
    <Dialog name="community-qr" title={title} wide onClose={onClose} backLabel={t("common.close")} buttons={<DeckButton primary onClick={onClose}>{t("community.deck.done")}</DeckButton>}>
      <div className="kdc-qr" data-testid="deck-qr-link">
        <div className="kdc-qr-code"><QrCode text={url} size={232} title={url} /></div>
        <div className="kdc-qr-text"><p className="deck-say">{text}</p><p className="deck-mono kdc-url">{url.replace(/^https:\/\//, "")}</p></div>
      </div>
    </Dialog>
  );
}

// ---------------------------------------------------------------- a post (K10)

function DeckPost({ post, onClose }: { post: SupportPost; onClose: () => void }) {
  const t = useT();
  const qc = useQueryClient();
  const s = useCommunity().data;
  const net = s ? s.online && s.relay === "ok" : true;
  const w = statusWord(post);
  // opening it marks its replies seen
  useEffect(() => { if (post.unread) api.communityPostSeen(post.id).then(() => refreshCommunity(qc)).catch(() => {}); }, []);
  const refresh = () => refreshCommunity(qc);
  return (
    <Dialog name="community-post" title={post.title} wide onClose={onClose} backLabel={t("common.close")}
      buttons={<>
        {post.status !== "solved" && <DeckButton disabled={!net} onClick={() => api.communityPostSolved(post.id).then(refresh).then(() => { toast.ok(t("community.posts.fixedDone")); onClose(); }).catch((e) => toast.error(e))}>{t("community.posts.fixed")}</DeckButton>}
        <HoldButton disabled={!net} onConfirm={() => api.communityPostDelete(post.id).then(refresh).then(() => { toast.ok(t("community.posts.deleted")); onClose(); }).catch((e) => toast.error(e))}>{t("community.deck.holdDelete")}</HoldButton>
      </>}>
      <div className="kdc-qr" data-testid="deck-post">
        <div className="kdc-qr-code"><QrCode text={post.url} size={200} title={post.url} /></div>
        <div className="kdc-qr-text">
          <p className="deck-line"><Word tone={w.tone}>{t(`community.posts.status.${w.key}`)}</Word> <span className="deck-dim">{t("community.posts.replies", { count: post.replies })}</span></p>
          <p>{post.lastReply ? (post.lastReply.team ? t("community.posts.lastReplyTeam", { name: post.lastReply.name, ago: ago(post.lastReply.at) }) : t("community.posts.lastReply", { name: post.lastReply.name, ago: ago(post.lastReply.at) })) : t("community.posts.noReply", { ago: ago(post.created) })}</p>
          <p className="deck-note">{t("community.deck.postQr")}</p>
        </div>
      </div>
    </Dialog>
  );
}
