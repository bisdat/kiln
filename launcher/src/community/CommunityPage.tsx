// /community: Kiln's Discord in the launcher, all of it optional (docs/kiln/launcher/discord.md; WIREFRAMES D2 to D4).
// Get help (send a report), your support posts and their replies, the tester programme, news and events, the Discord
// account, Rich Presence with a preview card, links, and "What is sent and kept".
// Opening the page reads the public news (and the known-problems list) from frostdev.io; signed in, also the account
// and the posts. Nothing about the player is sent while signed out.
import { MoreHorizontal } from "lucide-react";
import { useEffect, useState } from "react";
import { Navigate, useNavigate } from "react-router-dom";
import { useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import type { SupportPost } from "../api/commands";
import { useGame } from "../api/hooks";
import { useT } from "../i18n";
import { Button, Card, ConfirmDanger, EmptyState, Menu, Page, PageHeader, Skeleton, SplitPage, StatusPill, Toggle, errorText, toast } from "../ui";
import { INVITE, LINKS, ago, clockTime, cq, eventTime, openSendReport, sayLine, setCommunity, useCommunity, useCommunitySettings, useCommunityUi, useFeed, usePosts } from "./state";
import { statusWord } from "./parts";
import { PresenceSection } from "./Presence";
import { TesterSection } from "./Testers";
import "./community.css";

export const openUrl = (url: string) => { api.appOpenUrl(url).catch((e) => toast.error(e)); };

export default function CommunityPage() {
  const t = useT();
  const qc = useQueryClient();
  const st = useCommunity();
  const s = st.data;
  const acct = s?.account ?? null;
  const posts = usePosts(!!acct);
  const feed = useFeed();
  const { game } = useGame();
  const [forget, setForget] = useState(false);
  const [alsoDelete, setAlsoDelete] = useState(false);
  const net = s ? s.online && s.relay === "ok" : true;
  // the account and the membership are read again when the page opens
  useEffect(() => { if (acct) api.communityCheckMember().catch(() => {}); }, [!!acct]);
  const refresh = () => qc.invalidateQueries({ queryKey: ["community"] });

  return (
    <Page className="kc-page">
      <PageHeader title={t("community.title")} sub={sayLine(s)}
        meta={acct ? <StatusPill tone={acct.member ? "ok" : "warn"}>{acct.member ? t("community.account.member") : t("community.account.notMember")}</StatusPill> : undefined}
        actions={<Button onClick={() => openUrl(INVITE)} disabled={s ? !s.online : false} data-testid="kc-join">{acct?.member ? t("community.openDiscord") : t("community.join")}</Button>} />
      <SplitPage form aside={
        <>
          <Card title={t("community.account.title")} data-testid="kc-account">
            {acct ? (
              <div className="kc-acct">
                <span className="kc-avatar big" aria-hidden>{acct.name.slice(0, 1).toUpperCase()}</span>
                <div className="kc-acct-lines">
                  <b>{acct.name}</b>
                  <span className="k-hint">{acct.member ? t("community.account.member") : t("community.account.notMember")}{acct.tester ? ` · ${t("community.account.tester")}` : ""}</span>
                </div>
              </div>
            ) : (
              <>
                <p className="kc-p">{t("community.account.out")} {t("community.account.outHint")}</p>
                <div><Button onClick={() => useCommunityUi.getState().openSignIn()} disabled={!net} data-testid="kc-signin">{t("community.account.signIn")}</Button></div>
              </>
            )}
            {acct && (
              <div className="kc-links-row">
                <button type="button" className="k-link" data-testid="kc-signout" onClick={() => api.communitySignOut(false).then(refresh).then(() => toast.ok(t("community.account.signedOut"), t("community.account.signedOutDetail"))).catch((e) => toast.error(e))}>{t("community.account.signOut")}</button>
                <button type="button" className="k-link" data-testid="kc-forget" onClick={() => setForget(true)}>{t("community.account.forget")}</button>
              </div>
            )}
            {acct && <Toggle checked={!!s?.notify} onChange={(on) => { setCommunity(qc, { notify: on }).catch((e) => toast.error(e)); }} label={t("community.notify.label")} hint={t("community.notify.hint")} />}
          </Card>
          <PresenceSection name={acct?.name ?? "you"} />
          <Card title={t("community.links.title")}>
            <div className="kc-link-list">
              <button type="button" className="k-link" onClick={() => openUrl(LINKS.docs)}>{t("community.links.docs")}</button>
              <button type="button" className="k-link" onClick={() => openUrl(LINKS.issues)}>{t("community.links.issues")}</button>
              <button type="button" className="k-link" onClick={() => openUrl(LINKS.nexus)}>{t("community.links.nexus")}</button>
              <button type="button" className="k-link" onClick={() => openUrl(LINKS.download)}>{t("community.links.download")}</button>
              <button type="button" className="k-link" data-testid="kc-privacy-link" onClick={() => useCommunityUi.getState().setPrivacy(true)}>{t("community.account.whatIsSent")}</button>
            </div>
          </Card>
        </>
      }>
        <Card title={t("community.help.title")} sub={t("community.help.sub")} data-testid="kc-help">
          <p className="kc-p">{t("community.help.text")}</p>
          <div className="k-row" style={{ gap: 12 }}>
            <Button variant="primary" data-testid="kc-send" onClick={() => openSendReport("community", game ? { game: game.id } : null)}>{t("community.help.send")}</Button>
            <NavSave />
            {!acct && <span className="k-hint">{t("community.help.needsSignIn")}</span>}
          </div>
        </Card>

        <Card title={t("community.posts.title")} sub={acct && s && !net && s.postsAsOf ? t("community.posts.asOf", { time: clockTime(s.postsAsOf) }) : undefined} data-testid="kc-posts">
          {!acct ? <EmptyState title={t("community.posts.emptyOut")} />
            : posts.isLoading ? <Skeleton lines={2} />
            : !(posts.data ?? []).length ? <EmptyState title={t("community.posts.empty")} />
            : <div className="kc-posts">{posts.data!.map((p) => <PostRow key={p.id} p={p} net={net} />)}</div>}
        </Card>

        <TesterSection signedIn={!!acct} net={net} />

        <div className="kc-two">
          <Card title={t("community.news.title")} actions={<button type="button" className="k-link" onClick={() => openUrl(LINKS.download)}>{t("community.news.all")}</button>} data-testid="kc-news">
            {feed.isLoading ? <Skeleton lines={3} /> : feed.error || !feed.data ? <EmptyState title={t("community.news.offline")} /> : (
              <ul className="kc-feed">
                {feed.data.releases.map((r) => (
                  <li key={r.title}><button type="button" className="kc-feed-title" onClick={() => openUrl(r.url)}>{r.title}</button> <span className="k-muted">{ago(r.date)}</span><div className="kc-feed-line">{r.line}</div></li>
                ))}
              </ul>
            )}
          </Card>
          <Card title={t("community.events.title")} data-testid="kc-events">
            {feed.isLoading ? <Skeleton lines={2} /> : feed.error || !feed.data ? <EmptyState title={t("community.news.offline")} /> : !feed.data.events.length ? <EmptyState title={t("community.events.empty")} /> : (
              <ul className="kc-feed">
                {feed.data.events.map((e) => (
                  <li key={e.title}><button type="button" className="kc-feed-title" onClick={() => openUrl(e.url)}>{e.title}</button><div className="kc-feed-line">{eventTime(e.start)}. {e.where}.</div></li>
                ))}
              </ul>
            )}
          </Card>
        </div>
      </SplitPage>
      <ConfirmDanger open={forget} onClose={() => setForget(false)} title={t("community.privacy.forgetAsk")} word={t("common.confirmWord.forget")} action={t("community.account.forget")}
        message={<>
          <p>{t("community.privacy.forgetText")}</p>
          <label className="kc-tick"><input type="checkbox" className="k-check" checked={alsoDelete} onChange={(e) => setAlsoDelete(e.target.checked)} data-testid="kc-also-delete" /> {t("community.privacy.alsoDelete")}</label>
        </>}
        onConfirm={async () => {
          try { await api.communitySignOut(true, alsoDelete); } catch (e) { toast.error(errorText(e)); throw e; } finally { await refresh(); }
          toast.ok(t("community.account.signedOut"), t("community.account.signedOutDetail"));
        }} />
    </Page>
  );
}

function NavSave() {
  const t = useT();
  const nav = useNavigate();
  return <Button variant="ghost" onClick={() => nav("/logs")}>{t("community.help.save")}</Button>;
}

function PostRow({ p, net }: { p: SupportPost; net: boolean }) {
  const t = useT();
  const qc = useQueryClient();
  const [del, setDel] = useState(false);
  const w = statusWord(p);
  const line = p.lastReply ? (p.lastReply.team ? t("community.posts.lastReplyTeam", { name: p.lastReply.name, ago: ago(p.lastReply.at) }) : t("community.posts.lastReply", { name: p.lastReply.name, ago: ago(p.lastReply.at) }))
    : t("community.posts.noReply", { ago: ago(p.created) });
  // opening a post marks its replies seen
  const go = () => { openUrl(p.url); if (p.unread) api.communityPostSeen(p.id).then(() => qc.invalidateQueries({ queryKey: ["community"] })).catch(() => {}); };
  const refresh = () => qc.invalidateQueries({ queryKey: cq.posts }).then(() => qc.invalidateQueries({ queryKey: cq.status }));
  return (
    <div className={"kc-post" + (p.unread ? " hot" : "")} data-testid="kc-post" data-post={p.id}>
      <div className="kc-post-main">
        <div className="kc-post-title">{p.title}</div>
        <div className="kc-post-line k-hint">{line} {p.replies ? t("community.posts.replies", { count: p.replies }) + "." : ""}</div>
      </div>
      <StatusPill tone={w.tone}>{t(`community.posts.status.${w.key}`)}</StatusPill>
      <Button size="s" variant={p.unread ? "secondary" : "ghost"} onClick={go}>{t("community.posts.open")}</Button>
      <Menu label={t("community.posts.more", { title: p.title })} icon={<MoreHorizontal />} size="s" items={[
        { label: t("community.posts.copyLink"), onSelect: () => { navigator.clipboard?.writeText(p.url).then(() => toast.ok(t("community.posts.copied"))).catch(() => {}); } },
        { label: t("community.posts.fixed"), disabled: p.status === "solved" || !net, onSelect: () => { api.communityPostSolved(p.id).then(refresh).then(() => toast.ok(t("community.posts.fixedDone"), t("community.posts.fixedDetail"))).catch((e) => toast.error(e)); } },
        { label: t("community.posts.delete"), danger: true, disabled: !net, onSelect: () => setDel(true) },
      ]} />
      <ConfirmDanger open={del} onClose={() => setDel(false)} title={t("community.posts.deleteAsk")} word={t("common.confirmWord.delete")} action={t("common.delete")}
        message={<p>{t("community.posts.deleteText")}</p>}
        onConfirm={async () => {
          try { await api.communityPostDelete(p.id); await refresh(); toast.ok(t("community.posts.deleted")); } catch (e) { toast.error(errorText(e)); throw e; }
        }} />
    </div>
  );
}

/** The route: the page while it is shown; with "Show the Community page" off, Play. */
export function CommunityRoute() {
  const s = useCommunitySettings();
  if (!s.data) return null;
  return s.data.shown ? <CommunityPage /> : <Navigate to="/" replace />;
}
