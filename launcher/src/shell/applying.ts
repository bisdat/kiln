// (29.18, inbox 89) Whether a profile is being applied to a game right now, whatever started it: Play says "Applying
// profile…" and stays off until it ends or fails, on the desktop and the Deck (usePlayControl's `applying`).
// Two sources, read here and nowhere else:
// - the task list the launcher already keeps (ui/TaskProgress.tsx, fed by kiln://task and task_list at start): a task
//   whose kind writes a game's profile.json or copies, moves or replaces files in a game folder, queued or running.
//   Tasks name no game, so such a task holds Play in every game;
// - the activation guard (api/oneActivation.ts): profile_activate, profile_deactivate, profile_ensure_default and
//   library_add_to_profile for this game, which return only when they are done (Play's own start aside).
// Before 29.18 Play was refused only after the click (the guard), and the task-based activations not at all.
import type { Task } from "../api/commands";
import { useDirectActivations } from "../api/oneActivation";
import { useTasks } from "../ui/TaskProgress";

/** Task kinds that activate a profile in a game folder: library_add (a resumed or first profile, the add itself),
 *  modpack_install, found_add, nexus_install (they put mods into the active profile and write it), and runtime_install
 *  and runtime_rollback (Kiln's files, then the profile again). Cooks are not here: cook_run holds the game's own lock,
 *  which the backend's start refuses. */
export const PROFILE_TASKS: readonly string[] = ["library_add", "modpack_install", "found_add", "nexus_install"];
export const KILN_TASKS: readonly string[] = ["runtime_install", "runtime_rollback"];

export interface Applying {
  /** "profile": a profile is written into the game ("Applying profile…"); "kiln": Kiln's own files go in first
   *  ("Installing Kiln…"). */
  what: "profile" | "kiln";
  /** The task's title ("Add night-owl-1.2.0.zip", "Install Kiln 0.2.0-dev"); null for an activation that is no task. */
  title: string | null;
}

const busy = (task: Task) => task.state === "queued" || task.state === "running";

/** The oldest activation-like task that has not ended, or null. */
export function applyingTask(tasks: Record<string, Task>): Task | null {
  let first: Task | null = null;
  for (const task of Object.values(tasks)) {
    if (!busy(task) || (!PROFILE_TASKS.includes(task.kind) && !KILN_TASKS.includes(task.kind))) continue;
    if (!first || task.started < first.started) first = task;
  }
  return first;
}

/** What Play waits for: a running activation-like task, else a guarded activation of this game, else null. */
export function applyingNow(task: Pick<Task, "kind" | "title"> | null, direct: boolean): Applying | null {
  if (task) return { what: KILN_TASKS.includes(task.kind) ? "kiln" : "profile", title: task.title || null };
  return direct ? { what: "profile", title: null } : null;
}

/** applyingNow for the selected game, re-rendering only when the answer changes (not at each progress report). */
export function useApplying(gameId: string | null | undefined): Applying | null {
  const direct = useDirectActivations((s) => !!gameId && !!s.games[gameId]);
  const kind = useTasks((s) => applyingTask(s.tasks)?.kind ?? null);
  const title = useTasks((s) => applyingTask(s.tasks)?.title ?? null);
  return applyingNow(kind ? { kind, title: title ?? "" } : null, direct);
}
