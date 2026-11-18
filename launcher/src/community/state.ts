// The Discord features' shared state for both UIs: the queries (settings, status, posts, feed, tester), the dialogs
// any page can open (send a report, sign in, what is sent and kept) with the report's draft, which outlives the dialog
// and a sign-in round trip, and the live feed from the backend (kiln://community).
//
// Requests: the settings and the status are local (no request). Posts, news and the tester's plan reach the relay;
// they are asked for only while the Community page is open (and the backend refreshes the posts every 5 minutes while
// signed in). With the page hidden in Settings nothing here asks for anything that leaves the PC.
import { useEffect } from "react";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { create } from "zustand";
import * as api from "../api/commands";
import type { CommunitySettings, LogTarget, ProblemKind, ReportDraft, SendFrom } from "../api/commands";
import { t } from "../i18n";
import { toast } from "../ui";

export const INVITE = "https://discord.gg/7pjr8SMTfU";
export const PRIVACY_URL = "https://frostdev.io/kiln/privacy";
export const TERMS_URL = "https://frostdev.io/kiln/terms";
export const LINKS = {
  docs: "https://frostdev.io/kiln/docs",
  issues: "https://github.com/frostdev-ops/kiln/issues",
  nexus: "https://www.nexusmods.com/enshrouded/mods/301",
  download: "https://frostdev.io/kiln/download",
};

export const cq = {
  settings: ["community", "settings"] as const,
  status: ["community", "status"] as const,
  posts: ["community", "posts"] as const,
  feed: ["community", "feed"] as const,
  tester: ["community", "tester"] as const,
};

export const useCommunitySettings = () => useQuery({ queryKey: cq.settings, queryFn: api.communitySettingsGet, staleTime: 60_000 });
/** Whether the Community page, its tab, the reply count and "Send to Kiln support" show. False until known, so
 *  nothing hidden flashes up. */
export function useShown(): boolean {
  return useCommunitySettings().data?.shown === true;
}
export function useCommunity() {
  const shown = useShown();
  return useQuery({ queryKey: cq.status, queryFn: api.communityStatus, enabled: shown, refetchInterval: 60_000 });
}
/** The player's posts: asked for by the Community page (open) while signed in. */
export function usePosts(enabled: boolean) {
  return useQuery({ queryKey: cq.posts, queryFn: api.communityPosts, enabled, refetchInterval: 5 * 60_000, retry: false });
}
/** News and events: only the Community page asks, when it opens. */
export const useFeed = () => useQuery({ queryKey: cq.feed, queryFn: api.communityFeed, retry: false, staleTime: 10 * 60_000 });
export const useTester = (enabled = true) => useQuery({ queryKey: cq.tester, queryFn: api.testerStatus, enabled, retry: false });

/** Everything the page shows that may have changed, but not the public news (it is read once when the page opens). */
export const refreshCommunity = (qc: ReturnType<typeof useQueryClient>) =>
  qc.invalidateQueries({ predicate: (q) => q.queryKey[0] === "community" && q.queryKey[1] !== "feed" && q.queryKey[1] !== "prepare" });

export async function setCommunity(qc: ReturnType<typeof useQueryClient>, patch: Partial<Pick<CommunitySettings, "shown" | "presence" | "notify">>) {
  const s = await api.communitySettingsSet(patch);
  qc.setQueryData(cq.settings, s);
  await refreshCommunity(qc);
  return s;
}

/** Where a report was started: its preselected kind. */
const KIND_OF: Record<SendFrom, ProblemKind> = { logs: "noload", banner: "noload", crash: "crash", community: "noload", deck: "noload" };

/** `session`: the prepared report this dialog shows and sends (a new one for another place or a new report). */
interface SendState { from: SendFrom; target: LogTarget | null; step: "write" | "check" | "sent"; draft: ReportDraft; sentUrl: string | null; session: number }
let sendSession = 0;
const sameTarget = (a: LogTarget | null, b: LogTarget | null) => JSON.stringify(a) === JSON.stringify(b);
interface CommunityUi {
  send: SendState | null;
  signIn: { resumeSend: boolean } | null;
  privacy: boolean;
  openSend: (from: SendFrom, target: LogTarget | null, kind?: ProblemKind) => void;
  setSend: (s: Partial<SendState>) => void;
  setDraft: (d: Partial<ReportDraft>) => void;
  closeSend: () => void;
  openSignIn: (resumeSend?: boolean) => void;
  closeSignIn: () => void;
  setPrivacy: (on: boolean) => void;
}

export const useCommunityUi = create<CommunityUi>((set, get) => ({
  send: null,
  signIn: null,
  privacy: false,
  openSend: (from, target, kind) => {
    const was = get().send;
    // a draft started earlier and not sent is offered again (the same words)
    if (was && was.step !== "sent") {
      const same = was.from === from && sameTarget(was.target, target);
      set({ send: { ...was, step: "write", from, target, session: same ? was.session : ++sendSession } });
      return;
    }
    set({ send: { from, target, step: "write", sentUrl: null, session: ++sendSession, draft: { title: "", kind: kind ?? KIND_OF[from], text: "", zip: "team", when: null } } });
  },
  setSend: (s) => { const cur = get().send; if (cur) set({ send: { ...cur, ...s } }); },
  setDraft: (d) => { const cur = get().send; if (cur) set({ send: { ...cur, draft: { ...cur.draft, ...d } } }); },
  closeSend: () => {
    const cur = get().send;
    // a sent report is done; an unsent draft with words in it is kept for the next "Send to Kiln support"
    const keep = cur && cur.step !== "sent" && (cur.draft.title || cur.draft.text);
    set({ send: keep ? { ...cur, step: "write" } : null });
    useSendOpen.setState({ open: false });
  },
  openSignIn: (resumeSend = false) => set({ signIn: { resumeSend } }),
  closeSignIn: () => set({ signIn: null }),
  setPrivacy: (on) => set({ privacy: on }),
}));

/** Whether the send dialog is showing (the draft can outlive it). */
export const useSendOpen = create<{ open: boolean }>(() => ({ open: false }));
export function openSendReport(from: SendFrom, target: LogTarget | null, kind?: ProblemKind) {
  useCommunityUi.getState().openSend(from, target, kind);
  useSendOpen.setState({ open: true });
}

/** The backend's live feed: refreshes the queries. A reply's system notification comes from the backend (when the
 *  player asked for them); the browser mock, which has none, shows the launcher's own notice instead. Mounted once by
 *  each shell. */
export function useCommunityFeed() {
  const qc = useQueryClient();
  useEffect(() => {
    let off: (() => void) | undefined;
    let live = true;
    api.onKilnEvent("kiln://community", (e) => {
      void refreshCommunity(qc);
      if (e.kind !== "reply" || api.isTauri()) return;
      const st = qc.getQueryData<{ notify: boolean }>(cq.status);
      if (st?.notify) toast.info(t("community.notify.title"), t("community.notify.detail", { by: e.by ?? "", title: e.title ?? "" }),
        { actions: e.url ? [{ label: t("community.posts.open"), onClick: () => { api.appOpenUrl(e.url!).catch((x) => toast.error(x)); } }] : [] });
    }).then((f) => (live ? (off = f) : f()));
    return () => { live = false; off?.(); };
  }, [qc]);
}

/** "2 hours ago" from an ISO time (rough, as the rest of the launcher says it). */
export function ago(iso: string, now = Date.now()): string {
  const s = Math.max(0, (now - new Date(iso).getTime()) / 1000);
  if (s < 90) return "just now";
  const m = Math.round(s / 60);
  if (m < 60) return `${m} minutes ago`;
  const h = Math.round(m / 60);
  if (h < 36) return h === 1 ? "an hour ago" : `${h} hours ago`;
  const d = Math.round(h / 24);
  return d === 1 ? "yesterday" : `${d} days ago`;
}

export const clockTime = (iso: string | null) => (iso ? new Date(iso).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }) : "");
/** "Saturday 21 November, 20:00" in the player's time zone. */
export const eventTime = (iso: string) => {
  const d = new Date(iso);
  return `${d.toLocaleDateString([], { weekday: "long", day: "numeric", month: "long" })}, ${d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}`;
};

/** The `detail.kind` of a community error ("offline", "relay", "not_member", ...). */
export const errKind = (e: unknown): string | null => {
  const d = (e as { detail?: { kind?: string } })?.detail;
  return d?.kind ?? null;
};

/** The page's one sentence under its title. */
export function sayLine(s: api.CommunityStatus | undefined, deck = false): string {
  if (!s) return "";
  const acct = s.account;
  if (!s.online) return acct && !deck ? t("community.say.offline", { time: clockTime(s.postsAsOf) }) : t("community.say.offlineOut");
  if (s.relay !== "ok" && acct) return t("community.say.relay", { time: clockTime(s.postsAsOf) });
  if (!acct) return deck ? t("community.deck.sayOut") : t("community.say.out");
  return s.unread ? t("community.say.unread", { name: acct.name, count: s.unread }) : t("community.say.in", { name: acct.name });
}
