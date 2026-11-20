// Rich Presence (WIREFRAMES D13, K11): one switch, off by default, with the preview card beside it so the player sees
// exactly what others see. The backend shows it on Discord while the game runs with Kiln (src-tauri/src/community/
// presence.rs); this file also sends it what only the window knows: how many mods are on and the words.
import { useEffect, useState } from "react";
import { useQueryClient } from "@tanstack/react-query";
import * as api from "../api/commands";
import { useGame } from "../api/hooks";
import { useMods } from "../shell/useMods";
import { t as tt, useT } from "../i18n";
import { Card, Toggle, toast } from "../ui";
import { DeckButton, Dialog, Row, Word } from "../deck/ui";
import { setCommunity, useCommunity, useCommunitySettings } from "./state";
import { PresenceCard } from "./parts";

/** The preview's example time (the card is a preview; Discord counts the real one). */
const PREVIEW_TIME = "1:24:07";

async function turn(qc: ReturnType<typeof useQueryClient>, on: boolean) {
  try {
    await setCommunity(qc, { presence: on });
    toast.ok(on ? tt("community.presence.onToast") : tt("community.presence.offToast"));
  } catch (e) { toast.error(e); }
}

/** The desktop page's card. */
export function PresenceSection({ name }: { name: string }) {
  const t = useT();
  const qc = useQueryClient();
  const s = useCommunity().data;
  const { game } = useGame();
  const mods = useMods(game);
  const on = !!s?.presence.on;
  return (
    <Card title={t("community.presence.title")} data-testid="kc-presence">
      <Toggle checked={on} onChange={(v) => void turn(qc, v)} label={t("community.presence.label")} hint={t("community.presence.hint")} />
      {on && <span className={"k-hint" + (s!.presence.discordRunning ? "" : " k-tone-warn")} data-testid="kc-presence-state">{s!.presence.discordRunning ? t("community.presence.on") : t("community.presence.noDiscord")}</span>}
      <div className="kc-preview-label k-label">{t("community.presence.preview")}</div>
      <PresenceCard name={name} mods={mods.count?.on ?? 12} time={PREVIEW_TIME} dim={!on} />
      <span className="k-hint">{t("community.presence.never")}</span>
    </Card>
  );
}

/** The Deck tab's row and its dialog. */
export function DeckPresenceRow() {
  const t = useT();
  const s = useCommunity().data;
  const [open, setOpen] = useState(false);
  return (
    <>
      <Row id="presence" title={t("community.presence.title")} sub={t("community.deck.presenceSub")} a={t("common.open")} onClick={() => setOpen(true)}
        end={<Word tone={s?.presence.on ? (s.presence.discordRunning ? "ok" : "warn") : "idle"}>{s?.presence.on ? t("community.deck.on") : t("community.deck.off")}</Word>} />
      {open && <DeckPresence name={s?.account?.name ?? "you"} onClose={() => setOpen(false)} />}
    </>
  );
}

function DeckPresence({ onClose, name }: { onClose: () => void; name: string }) {
  const t = useT();
  const qc = useQueryClient();
  const s = useCommunity().data;
  const { game } = useGame();
  const mods = useMods(game);
  const on = !!s?.presence.on;
  return (
    <Dialog name="community-presence" title={t("community.presence.title")} wide onClose={onClose} backLabel={t("common.close")}
      buttons={<DeckButton primary onClick={() => void turn(qc, !on)}>{on ? t("community.deck.turnOff") : t("community.deck.turnOn")}</DeckButton>}>
      <div className="kdc-presence" data-testid="deck-presence">
        <PresenceCard name={name} mods={mods.count?.on ?? 12} time={PREVIEW_TIME} dim={!on} />
        <div>
          <p className="deck-line"><Word tone={on ? (s?.presence.discordRunning ? "ok" : "warn") : "idle"}>{on ? (s?.presence.discordRunning ? t("community.presence.on") : t("community.presence.noDiscordDeck")) : t("community.presence.off")}</Word></p>
          <p>{t("community.presence.hint")}</p>
          <p className="deck-dim">{t("community.presence.never")}</p>
        </div>
      </div>
    </Dialog>
  );
}

/** Mounted by the shells while the Community page is shown: sends the words in the launcher's language (Rich
 *  Presence's and the reply notification's) and, while Rich Presence is on, the mods on (the Play page's count, every
 *  kind) whenever they change. */
export function PresenceFacts() {
  const s = useCommunitySettings().data;
  if (!s?.shown) return null;
  return s.presence ? <PresenceFactsOn /> : <FactsPush count={null} />;
}

function PresenceFactsOn() {
  const { game } = useGame();
  const mods = useMods(game);
  return <FactsPush count={mods.count ? mods.count.on : null} />;
}

function FactsPush({ count }: { count: number | null }) {
  const t = useT();
  const texts: api.PresenceTexts = {
    details: t("community.presence.details"), stateOne: t("community.presence.state.one"), stateOther: t("community.presence.state.other"),
    getKiln: t("community.presence.getKiln"), joinDiscord: t("community.presence.joinDiscord"),
  };
  const notify: api.NotifyTexts = { title: t("community.notify.title"), detail: t("community.notify.detail"), open: t("community.posts.open") };
  const key = JSON.stringify([count, texts, notify]);
  useEffect(() => { api.communityPresenceFacts(count, texts, notify).catch(() => {}); }, [key]);
  return null;
}
