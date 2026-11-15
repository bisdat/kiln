import { useNavigate } from "react-router-dom";
import type { PlayControl } from "./play";
import { Banner, Button } from "../ui";
import { DeckButton } from "../deck/ui";
import { stopFilter, stopSentence, stopShowsReason } from "./stoppedStart";

export function StoppedStartNotice({ ctl, deck = false }: { ctl: PlayControl; deck?: boolean }) {
  const nav = useNavigate();
  const s = ctl.stoppedStart;
  if (!s) return null;
  const review = () => nav(`${deck ? "/deck/mods" : "/"}?filter=${encodeURIComponent(stopFilter(s.mods))}`);
  const body = <><p>{stopSentence(s)}</p>{stopShowsReason(s) && <p>{s.reason}</p>}<p>{s.advice}</p></>;
  return <section role="region" aria-label="Kiln stopped the start" className="k-stop-notice" tabIndex={deck ? undefined : 0} style={{ overflowWrap: "anywhere", flexShrink: 0, maxHeight: deck ? undefined : "min(280px, 42vh)", overflowY: deck ? undefined : "auto" }}>
    {deck ? <><p role="status">Kiln stopped the start</p>{body}<div className="deck-home-actions">
      {s.mods.length > 0 && <DeckButton id="stop-mods" onClick={review}>Review these mods</DeckButton>}
      <DeckButton id="stop-dismiss" onClick={ctl.dismissStop}>Dismiss</DeckButton>
    </div></> : <Banner tone="warn" title="Kiln stopped the start" actions={<>
      {s.mods.length > 0 && <Button size="s" onClick={review}>Review these mods</Button>}
      <Button size="s" onClick={ctl.dismissStop}>Dismiss</Button>
    </>}>{body}</Banner>}
  </section>;
}
