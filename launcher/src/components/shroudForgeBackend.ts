// Settings' ShroudForge card, its rules apart from React: which rows it offers and when a choice
// may be written.
import type { GameId, SfBackend, SfBackendCard, SfBackendPreview } from "../api/commands";

/** What the card offers, in order. ShroudForge 1.8 is never one of them: it is held by the release policy, which no
 *  setting, choice or preview lifts. */
export const SF_CHOICES: readonly SfBackend[] = ["hosted", "installed", "kiln"];

/** The backend the card marks: a choice or the older switch's, hosted when nothing is set, none for a value no reader takes. */
export function chosenOf(card: SfBackendCard): SfBackend | null {
  const s = card.setting;
  return s.kind === "default" ? "hosted" : s.kind === "invalid" ? null : s.backend;
}

export interface ChoiceRow {
  backend: SfBackend;
  checked: boolean;
  /** Cannot be chosen now (the game runs, kiln.json cannot be read, or the interpreter is held). */
  disabled: boolean;
  /** Kiln's interpreter while it is held: why. */
  held?: string;
  isDefault: boolean;
}

/** The rows of the card. */
export function choiceRows(card: SfBackendCard, running: boolean): ChoiceRow[] {
  const chosen = chosenOf(card);
  const locked = running || !!card.unreadable;
  return SF_CHOICES.map((backend) => {
    const held = backend === "kiln" ? card.interpreterHeld : undefined;
    return { backend, checked: backend === chosen, disabled: locked || !!held, ...(held ? { held } : {}), isDefault: backend === "hosted" };
  });
}

/** A change the player asked for: one game, one backend (null: back to the default), on the kiln.json bytes the card read. */
export interface Draft {
  gameId: GameId;
  backend: SfBackend | null;
  revision: string;
}

/** The draft of choosing `backend` (null: back to the default) on this card; null when there is nothing to ask (the row
 *  already chosen, the default again, a row that cannot be chosen). */
export function draftFor(card: SfBackendCard, backend: SfBackend | null, running: boolean): Draft | null {
  if (running || card.unreadable) return null;
  if (backend === null) return card.setting.kind === "default" ? null : { gameId: card.gameId, backend, revision: card.revision };
  const row = choiceRows(card, running).find((r) => r.backend === backend);
  if (!row || row.disabled || row.checked) return null;
  return { gameId: card.gameId, backend, revision: card.revision };
}

/** Save writes the draft only when its preview answers exactly this draft, for the game the card shows now, on the bytes
 *  the card read, and says it can be written. A late answer for another game, another choice or older bytes never saves. */
export function canSave(draft: Draft | null, shownGameId: GameId | null | undefined, card: SfBackendCard | undefined,
  preview: SfBackendPreview | undefined): draft is Draft {
  if (!draft || !card || !preview || !shownGameId) return false;
  if (draft.gameId !== shownGameId || card.gameId !== shownGameId || preview.gameId !== shownGameId) return false;
  if (preview.backend !== draft.backend || preview.revision !== draft.revision || card.revision !== draft.revision) return false;
  return !preview.refused && !!preview.next && !preview.unchanged;
}
