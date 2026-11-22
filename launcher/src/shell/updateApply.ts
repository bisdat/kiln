// The launcher update's own steps, as rules (shell/UpdateNotice.tsx runs them):
// - the restart after an update the player started: by itself, after a short countdown, unless the game runs or Pack
//   Studio or an editor has unsaved work (then today's Later and Restart, and the notice says why it waited); a running
//   task is waited for (Restart waits for it too), then the countdown;
// - after the restart, the Kiln runtime the update brought goes into the game by itself while the game is closed,
//   through the ordinary install (its backup, its refusals); while the game runs, Update game is held and it goes in
//   when the game closes.
import type { GameStatus } from "../api/commands";
import type { UnsavedKind } from "./unsaved";

/** What holds the restart: the game, unsaved work (both hold it for good: the player restarts), a start of the game
 *  under way or a task (both waited for; a start that brings the game holds it for good then). */
export type RestartHold = { kind: "game" } | { kind: "launch" } | { kind: "unsaved"; who: UnsavedKind } | { kind: "task"; title: string } | null;

/** `running`: the game runs. `launching`: Play was pressed, the warning before Play is open, or Steam was asked and the
 *  game's process is not there yet (the backend's update_restart refuses then too). */
export function restartHold(o: { running: boolean; launching?: boolean; unsaved: UnsavedKind | null; task: string | null }): RestartHold {
  if (o.running) return { kind: "game" };
  if (o.launching) return { kind: "launch" };
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
