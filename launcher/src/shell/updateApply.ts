// The launcher update's own steps, as rules (shell/UpdateNotice.tsx runs them):
// - the restart after an update the player started: by itself, after a short countdown, unless the game runs or Pack
//   Studio or an editor has unsaved work (then today's Later and Restart, and the notice says why it waited); a running
//   task is waited for (Restart waits for it too), then the countdown;
// - after the restart, the Kiln runtime the update brought goes into the game by itself while the game is closed,
//   through the ordinary install (its backup, its refusals); while the game runs, Update game is held and it goes in
//   when the game closes.
import type { GameStatus } from "../api/commands";
import type { UnsavedKind } from "./unsaved";

/** The player's Later (or a hold for good: the game or unsaved work held it) per ready version. */
export type RestartDecision = "held" | "stopped";
const DECIDED = "kiln.update.decided";

/** (0.2.0-dev.31.13) The decisions of this run of the launcher (sessionStorage): a page loaded again after its web process
 *  ended (src-tauri/src/webproc.rs) does not count down to a restart the player put off with Later. A new start of the
 *  launcher forgets them, as before. Anything unreadable is no decision. */
export function loadDecided(store: Pick<Storage, "getItem"> | null): Partial<Record<string, RestartDecision>> {
  try {
    const v: unknown = JSON.parse(store?.getItem(DECIDED) ?? "{}");
    if (!v || typeof v !== "object" || Array.isArray(v)) return {};
    return Object.fromEntries(Object.entries(v).filter(([, d]) => d === "held" || d === "stopped")) as Partial<Record<string, RestartDecision>>;
  } catch { return {}; }
}

export function saveDecided(store: Pick<Storage, "setItem"> | null, decided: Partial<Record<string, RestartDecision>>): void {
  try { store?.setItem(DECIDED, JSON.stringify(decided)); } catch { /* no storage: this page only, as before 31.13 */ }
}

/** The page's sessionStorage, or null where there is none. */
export function sessionStore(): Storage | null {
  try { return typeof sessionStorage === "undefined" ? null : sessionStorage; } catch { return null; }
}

/** What holds the restart: the game, unsaved work (both hold it for good: the player restarts), a start of the game
 *  under way, the save editor writing a save (a save or a restore: the close guard asks then too) or a task (each waited
 *  for; a start that brings the game holds it for good then). */
export type RestartHold = { kind: "game" } | { kind: "launch" } | { kind: "writing" } | { kind: "unsaved"; who: UnsavedKind } | { kind: "task"; title: string } | null;

/** `running`: the game runs. `launching`: Play was pressed, the warning before Play is open, or Steam was asked and the
 *  game's process is not there yet (the backend's update_restart refuses then too). `writing`: the save editor is writing
 *  a save (editor/kiln/pending.ts `busy`: a save, or a restore of a backup, which has no unsaved changes). */
export function restartHold(o: { running: boolean; launching?: boolean; writing?: boolean; unsaved: UnsavedKind | null; task: string | null }): RestartHold {
  if (o.running) return { kind: "game" };
  if (o.launching) return { kind: "launch" };
  if (o.writing) return { kind: "writing" };
  if (o.unsaved) return { kind: "unsaved", who: o.unsaved };
  if (o.task) return { kind: "task", title: o.task };
  return null;
}

/** The runtime the update brought, to put into the game by itself: the launcher's own (`shipped`, the update status's
 *  shippedRuntime) when the game's Kiln is older (upgradeAvailable) and the game has Kiln. Any other runtime (a newer one
 *  in the Library, the older one of a switch back from a test build) stays a manual offer. */
export function runtimeToApply(status: GameStatus | undefined, shipped: string | null | undefined): string | null {
  const rt = status?.runtime;
  const next = rt?.installed ? rt.upgradeAvailable ?? null : null;
  return next && shipped && next === shipped ? next : null;
}

/** One automatic install per update and game (a second runtime never follows by itself). */
export const applyKey = (to: string, gameId: string) => `${to}|${gameId}`;

/** Why it is not put in yet: the game's status has not loaded, the game runs, Steam is updating it. */
export type ApplyWait = "loading" | "running" | "steam" | null;
export function applyWait(status: GameStatus | undefined): ApplyWait {
  if (!status) return "loading";
  if (status.running) return "running";
  if (status.steamUpdating) return "steam";
  return null;
}

/** The keys the automatic install already tried (applyKey), in this session: shell/UpdateNotice.tsx's install by itself
 *  tries each once, and Play's Update game marks its own try, so a failed click is not followed by a try by itself (a
 *  click before the update status answered has no key yet: the install by itself also skips a Kiln whose install
 *  already failed in this session). */
export const autoTried = new Set<string>();

/** (0.2.0-dev.31.12) Play's "Update game" banner (pages/PlayPage.tsx): the game's Kiln is older than the one this
 *  launcher has for it (InstallState.upgradeAvailable: in a release build the runtime the launcher ships, in a test
 *  build the newest offered bundle), the same in the Nexus build (its runtime ships too; only the self-update is gone).
 *  Null when the game has no Kiln or an equal or newer one (no offer), after a switch back (Play's own line puts the
 *  launcher's in), and while the channel notice speaks for it (`channel`: a test build of the launcher with the release's
 *  Kiln in the game). `hold`: why Update game is held (the game runs or starts, Steam updates it, a change runs);
 *  `failed`: the last change of this game failed while putting this very version in (the banner says its short reason). */
export interface GameBehind {
  has: string;
  next: string;
  hold: "running" | "starting" | "steam" | "applying" | null;
  failed: boolean;
}
export function gameBehind(status: GameStatus | undefined, o: { channel: boolean; starting?: boolean; change?: { state: string; version: string | null } | null }): GameBehind | null {
  const rt = status?.runtime;
  const has = rt?.installed ? rt.receipt?.runtime.version ?? null : null;
  const next = rt?.upgradeAvailable ?? null;
  if (!status || !has || !next || next === has || rt?.switchBack || o.channel) return null;
  const c = o.change ?? null;
  const hold = c?.state === "running" ? "applying" : status.running ? "running" : o.starting ? "starting" : status.steamUpdating ? "steam" : null;
  const failed = c?.state === "failed" && c.version === next;
  return { has, next, hold, failed };
}
