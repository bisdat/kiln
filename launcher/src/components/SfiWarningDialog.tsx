// the warning before Play: what will not run this start, for the player to read before
// the game opens. One dialog at a time (shell/sfiWarnings.ts holds the queue); Cancel has the focus and is the default
// (Enter on it, Escape, a click beside the dialog, the Deck's B all cancel); only "Continue without these mods"
// confirms. Every warning is listed in full in a scrolling list the keyboard can reach; advisories (shown, never
// acknowledged) sit apart and claim no package is skipped. Nothing here saves, deletes or switches anything off.
import { useGames } from "../api/hooks";
import type { Preflight } from "../api/commands";
import { Banner, Button, Modal } from "../ui";
import { DeckButton, Dialog } from "../deck/ui";
import { useT } from "../i18n";
import { advisoryText, strongest, useStartWarningHost, warningText } from "../shell/sfiWarnings";

function Body({ p, round }: { p: Preflight; round: number }) {
  const t = useT();
  const games = useGames();
  const game = games.data?.find((g) => g.id === p.gameId);
  const lead = strongest(p.warnings);
  const items = p.warnings.map(warningText);
  const notes = (p.advisories ?? []).map(advisoryText);
  return (
    <div className="k-col sfi-warn" style={{ gap: 10 }} data-testid="sfi-warning" data-game={p.gameId} data-round={round} data-lead={lead}>
      <div className="k-hint" data-testid="sfi-warning-game">{t("sfBackend.startWarning.game", { path: game?.path ?? p.gameId })}</div>
      {round > 0 && <Banner tone="warn" title={t("sfBackend.startWarning.changedTitle")}>{t("sfBackend.startWarning.changed")}</Banner>}
      <Banner tone={lead === "plain" ? "warn" : "err"} title={t(`sfBackend.startWarning.lead.${lead}.title`)}>
        <span data-testid="sfi-warning-lead">{t(`sfBackend.startWarning.lead.${lead}.text`)}</span>
      </Banner>
      <ul className="sfi-warn-list" data-testid="sfi-warning-list" aria-label={t("sfBackend.startWarning.listAria", { count: items.length })} tabIndex={0}
        style={{ maxHeight: 300, overflowY: "auto", margin: 0, padding: "0 4px 0 0", listStyle: "none", display: "flex", flexDirection: "column", gap: 8 }}>
        {items.map((w) => (
          <li key={w.key} data-testid="sfi-warning-item" data-save={w.save ? "" : undefined}>
            <strong>{w.title}</strong>
            {w.lines.map((l, i) => <div key={i} className="k-hint" style={{ whiteSpace: "pre-wrap", overflowWrap: "anywhere" }}>{l}</div>)}
          </li>
        ))}
      </ul>
      {notes.length > 0 && (
        <section aria-label={t("sfBackend.startWarning.notesAria")} data-testid="sfi-warning-notes" style={{ borderTop: "1px solid var(--k-line, #3334)", paddingTop: 8 }}>
          {notes.map((n) => (
            <div key={n.key} data-testid="sfi-warning-note">
              <strong>{n.title}</strong>
              {n.lines.map((l, i) => <div key={i} className="k-hint" style={{ overflowWrap: "anywhere" }}>{l}</div>)}
            </div>
          ))}
        </section>
      )}
      <div className="k-hint">{t("sfBackend.startWarning.nothingChanged")}</div>
    </div>
  );
}

/** The desktop dialog (mounted once in the shell). */
export function SfiWarningHost() {
  const t = useT();
  const ask = useStartWarningHost();
  if (!ask) return null;
  const no = () => ask.answer(false);
  return (
    <Modal key={ask.id} open title={t("sfBackend.startWarning.title")} onClose={no} width={600}
      footer={<>
        <Button data-autofocus onClick={no} data-testid="sfi-warning-cancel">{t("common.cancel")}</Button>
        <Button variant="danger" onClick={() => ask.answer(true)} data-testid="sfi-warning-continue">{t("sfBackend.startWarning.continue")}</Button>
      </>}>
      <Body p={ask.preflight} round={ask.round} />
    </Modal>
  );
}

/** The Deck's dialog (mounted once in DeckApp): B cancels, A presses the focused button, Cancel focused first. */
export function DeckSfiWarningHost() {
  const t = useT();
  const ask = useStartWarningHost();
  if (!ask) return null;
  const no = () => ask.answer(false);
  return (
    <Dialog key={ask.id} name="sfi-warning" title={t("sfBackend.startWarning.title")} onClose={no} backLabel={t("common.cancel")} wide
      buttons={<>
        <DeckButton autoFocus onClick={no}>{t("common.cancel")}</DeckButton>
        <DeckButton danger onClick={() => ask.answer(true)}>{t("sfBackend.startWarning.continue")}</DeckButton>
      </>}>
      <Body p={ask.preflight} round={ask.round} />
    </Dialog>
  );
}
