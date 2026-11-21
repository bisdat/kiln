// Pieces both UIs use: the QR code, the Rich Presence preview card, a post's state word, the problem kinds, the
// "Send to Kiln support" button of the entry points, the reply count after "Community", and Settings' switch.
import { useMemo } from "react";
import { useQueryClient } from "@tanstack/react-query";
import type { LogTarget, PostStatus, ProblemKind, SendFrom, SupportPost } from "../api/commands";
import { Button, Mark, Toggle, toast, type ButtonProps } from "../ui";
import { t, useT } from "../i18n";
import { qrEncode, qrPath } from "./qr";
import { DeckButton, Dialog, Row, Word } from "../deck/ui";
import { openSendReport, setCommunity, useCommunity, useCommunitySettings, useShown } from "./state";

export const KINDS: ProblemKind[] = ["noload", "mod", "crash", "launcher", "server", "other"];

/** A QR code for a phone: dark modules on the Hearth's paper, a 4-module quiet zone. */
export function QrCode({ text, size = 220, title }: { text: string; size?: number; title?: string }) {
  const qr = useMemo(() => { try { return qrEncode(text); } catch { return null; } }, [text]);
  if (!qr) return null;
  const full = qr.size + 8;
  return (
    <svg className="kc-qr" width={size} height={size} viewBox={`0 0 ${full} ${full}`} role="img" aria-label={title ?? text} shapeRendering="crispEdges" data-qr={text}>
      <rect width={full} height={full} fill="#f5e9db" rx="1.2" />
      <path d={qrPath(qr)} fill="#170803" />
    </svg>
  );
}

/** What others see on the player's Discord profile while they play (drawn by the launcher in neutral greys, not
 *  Discord's own picture and without its marks). */
export function PresenceCard({ name, mods, time, dim, pack }: { name: string; mods: number; time: string; dim?: boolean; pack?: boolean }) {
  const t = useT();
  return (
    <div className={"kc-presence" + (dim ? " dim" : "")} role="group" aria-label={t("community.presence.preview")} data-testid="kc-presence-card">
      <div className="kc-presence-user">
        <span className="kc-avatar" aria-hidden>{name.slice(0, 1).toUpperCase()}</span>
        <span className="kc-presence-name">{name}</span>
      </div>
      <div className="kc-presence-head">{t("community.presence.playing")}</div>
      <div className="kc-presence-act">
        <span className="kc-presence-art" aria-hidden><Mark size={34} /></span>
        <span className="kc-presence-lines">
          <b>{t("community.presence.name")}</b>
          <span>{t("community.presence.details")}</span>
          <span data-testid="kc-presence-card-state">{pack ? t("community.presence.pack.state", { count: mods, pack: t("community.presence.pack.example") }) : t("community.presence.state", { count: mods })}</span>
          <span className="kc-presence-time">{t("community.presence.elapsed", { time })}</span>
        </span>
      </div>
      <div className="kc-presence-btns">
        <span>{t("community.presence.getKiln")}</span>
        <span data-testid="kc-presence-card-second">{pack ? t("community.presence.pack.getPack") : t("community.presence.joinDiscord")}</span>
      </div>
    </div>
  );
}

export type StatusTone = "ok" | "idle" | "ember" | "info";
/** A post's state word: Waiting, New reply (ember, unread), Replied, Solved (celadon), Closed. */
export function statusWord(p: Pick<SupportPost, "status" | "unread">): { tone: StatusTone; key: string } {
  const map: Record<PostStatus, { tone: StatusTone; key: string }> = {
    waiting: { tone: "idle", key: "waiting" }, replied: { tone: p.unread ? "ember" : "info", key: p.unread ? "replied" : "repliedSeen" },
    solved: { tone: "ok", key: "solved" }, closed: { tone: "idle", key: "closed" },
  };
  return map[p.status];
}

/** "Send to Kiln support" next to a problem (a banner, the crash notice, the Logs page): nothing while the Community
 *  page is hidden in Settings. */
export function SendToSupport({ from, target, kind, size = "s", variant = "ghost", testId }: {
  from: SendFrom; target: LogTarget | null; kind?: ProblemKind; size?: ButtonProps["size"]; variant?: ButtonProps["variant"]; testId?: string;
}) {
  const t = useT();
  if (!useShown()) return null;
  return <Button size={size} variant={variant} data-testid={testId ?? "send-to-support"} onClick={() => openSendReport(from, target, kind)}>{t("community.banner.send")}</Button>;
}

/** New replies from Kiln support: a flame numeral after "Community" (desktop header and the Deck's tab). */
export function NavCount() {
  const t = useT();
  const n = useCommunity().data?.unread ?? 0;
  return n ? <span className="kc-nav-count" data-testid="nav-community-count" aria-label={t("community.navCount", { count: n })}>{n}</span> : null;
}

/** Settings: "Show the Community page" (on by default). */
export function CommunityToggle() {
  const t = useT();
  const qc = useQueryClient();
  const s = useCommunitySettings();
  return (
    <div data-testid="community-shown">
      <Toggle checked={s.data?.shown !== false} disabled={!s.data} onChange={(on) => { setCommunity(qc, { shown: on }).catch((e) => toast.error(e)); }}
        label={t("community.settings.show")} hint={t("community.settings.showHint")} />
    </div>
  );
}

/** The Deck's Settings row for "Show the Community page". */
export function DeckCommunityRow() {
  const t = useT();
  const qc = useQueryClient();
  const s = useCommunitySettings();
  const on = s.data?.shown !== false;
  return (
    <Row id="set-community" title={t("community.settings.show")} a={t("deck.shared.change")} disabled={!s.data} sub={t("community.settings.showHint")}
      end={<Word tone="ember">{on ? t("deck.shared.on") : t("deck.shared.off")}</Word>}
      onClick={() => { setCommunity(qc, { shown: !on }).catch((e) => toast.error(e)); }} />
  );
}

/** The i18n key part of a kind ("other" reads as a plural form to the i18n check, so its texts are "something"). */
export const kindKey = (k: ProblemKind) => (k === "other" ? "something" : k);
/** The i18n key part of a Deck "when" pick ("other" is "another"). */
export const whenKey = (w: string) => (w === "other" ? "another" : w);

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

/** Deck Home's "Send to Kiln support" next to a problem (nothing while the Community page is hidden). */
export function DeckSendToSupport({ target, kind }: { target: LogTarget | null; kind: ProblemKind }) {
  const t = useT();
  if (!useShown()) return null;
  return <DeckButton id="send-support" onClick={() => openSendReport("deck", target, kind)}>{t("community.banner.send")}</DeckButton>;
}

/** "38 minutes", "5 hours": how long until the relay takes a report again. */
export function waitWords(minutes: number): string {
  const m = Math.max(1, Math.round(minutes));
  return m < 90 ? t("community.banner.afterMinutes", { count: m }) : t("community.banner.afterHours", { count: Math.round(m / 60) });
}

/** The words for a report the relay's sending limits refused (`detail` of a `rate_limited` error): the hour's and the
 *  day's limits per account, or the network's (docs/relay-api.md). */
export function rateText(d: Record<string, unknown>): string {
  const wait = waitWords(typeof d.waitMin === "number" ? d.waitMin : 60);
  if (d.limit === "network") return t("community.send.err.rateNetwork", { wait });
  if (d.limit === "day") return t("community.send.err.rateDay", { sent: typeof d.sent === "number" ? d.sent : 10, wait });
  return t("community.send.err.rate", { sent: typeof d.sent === "number" ? d.sent : 3, wait });
}

/** The words for Discord's refusal or rate limit (`detail` of a `discord` error: docs/kiln/launcher/discord.md), for a
 *  report or for test results. Discord's reason is shown as Discord gave it (English), without its last full stop. */
export function discordWords(d: Record<string, unknown>, what: "report" | "results"): { title: string; text: string } {
  const k = what === "report" ? "community.send.err" : "community.tester.plan";
  if (d.limited === true) return { title: t("community.send.err.discordLimitTitle"), text: t(`${k}.discordLimit`, { wait: waitWords(typeof d.waitMin === "number" ? d.waitMin : 1) }) };
  const reason = typeof d.reason === "string" ? d.reason.trim().replace(/\.$/, "") : "";
  return { title: t(`${k}.discordTitle`), text: reason ? t(`${k}.discord`, { reason }) : t(`${k}.discordNoReason`) };
}

/** The post's versions line as the relay writes it: "Kiln 0.2.0 · Launcher 0.2.0 · Arch Linux · game build 1 · 12 mods on". */
export const versionsLine = (v: [string, string][]) => v.map(([k, x]) => (k ? `${k} ${x}` : x)).join(" · ");
