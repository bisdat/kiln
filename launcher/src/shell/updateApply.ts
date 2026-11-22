// The launcher update's own steps, as rules (shell/UpdateNotice.tsx runs them):
// - the restart after an update the player started: by itself, after a short countdown, unless the game runs or Pack
//   Studio or an editor has unsaved work (then today's Later and Restart, and the notice says why it waited); a running
//   task is waited for (Restart waits for it too), then the countdown;
// - after the restart, the Kiln runtime the update brought goes into the game by itself while the game is closed,
//   through the ordinary install (its backup, its refusals); while the game runs, Update game is held and it goes in
//   when the game closes.
import type { GameStatus } from "../api/commands";
import type { UnsavedKind } from "./unsaved";

/** What holds the restart: the game, unsaved work (both hold it for good: the player restarts), or a task (waited for). */
export type RestartHold = { kind: "game" } | { kind: "unsaved"; who: UnsavedKind } | { kind: "task"; title: string } | null;

/** `running`: the game runs, or its status is not known yet (an update is never ready before it is). */
export function restartHold(o: { running: boolean; unsaved: UnsavedKind | null; task: string | null }): RestartHold {
  if (o.running) return { kind: "game" };
  if (o.unsaved) return { kind: "unsaved", who: o.unsaved };
  if (o.task) return { kind: "task", title: o.task };
  return null;
}

/** The runtime the update brought, to put into the game by itself: the launcher's newer Kiln (upgradeAvailable) for a
 *  game that has Kiln. The older one of a switch back from a test build is not (Update game and Play put it in). */
export function runtimeToApply(status: GameStatus | undefined): string | null {
  const rt = status?.runtime;
  return rt?.installed ? rt.upgradeAvailable ?? null : null;
}

/** Why it is not put in yet: the game's status has not loaded, the game runs, Steam is updating it. */
export type ApplyWait = "loading" | "running" | "steam" | null;
export function applyWait(status: GameStatus | undefined): ApplyWait {
  if (!status) return "loading";
  if (status.running) return "running";
  if (status.steamUpdating) return "steam";
  return null;
}
