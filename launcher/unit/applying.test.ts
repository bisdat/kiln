// Inbox 89 (29.18): Play waits while a profile is being applied, whatever started it (src/shell/applying.ts): an
// activation-like task in the task list, or a guarded activation of the game (src/api/oneActivation.ts); Play's own
// start is not one.
import { beforeEach, expect, test } from "bun:test";
import type { Task } from "../src/api/commands";
import { oneActivation, useDirectActivations } from "../src/api/oneActivation";
import { applyingNow, applyingTask, applyingWord, KILN_TASKS, PROFILE_TASKS, UNINSTALL_TASKS } from "../src/shell/applying";
import { useTasks } from "../src/ui/TaskProgress";

const task = (id: string, kind: string, state: Task["state"], started = "2026-11-18T20:00:00Z"): Task =>
  ({ id, kind, title: `${kind} ${id}`, state, progress: null, message: null, started, finished: null });

beforeEach(() => {
  useTasks.setState({ tasks: {}, order: [] });
  useDirectActivations.setState({ games: {} });
});

test("every activation-like task kind holds Play while it is queued or running, and none after it ended", () => {
  for (const kind of [...PROFILE_TASKS, ...KILN_TASKS, ...UNINSTALL_TASKS]) {
    for (const state of ["queued", "running"] as const) expect(applyingTask({ a: task("a", kind, state) })?.kind).toBe(kind);
    for (const state of ["done", "failed", "canceled"] as const) expect(applyingTask({ a: task("a", kind, state) })).toBeNull();
  }
  expect([...PROFILE_TASKS, ...KILN_TASKS, ...UNINSTALL_TASKS].sort())
    .toEqual(["found_add", "library_add", "modpack_install", "nexus_install", "runtime_install", "runtime_rollback", "runtime_uninstall"]);
});

test("tasks that write no profile into a game do not hold Play", () => {
  for (const kind of ["library_import", "cook_run", "boot_snapshot_run", "vendor_download", "server_mods_sync", "update_install", "app_assets_import"])
    expect(applyingTask({ a: task("a", kind, "running") })).toBeNull();
});

test("the oldest running one is said; Kiln's install says so, a profile write says Applying", () => {
  const tasks = { b: task("b", "library_add", "running", "2026-11-18T20:00:05Z"), a: task("a", "runtime_install", "running", "2026-11-18T20:00:01Z"), c: task("c", "modpack_install", "done", "2026-11-18T19:00:00Z") };
  expect(applyingTask(tasks)?.id).toBe("a");
  expect(applyingNow(applyingTask(tasks), false)).toEqual({ what: "kiln", title: "runtime_install a" });
  expect(applyingNow(task("x", "found_add", "running"), false)).toEqual({ what: "profile", title: "found_add x" });
  expect(applyingNow(null, true)).toEqual({ what: "profile", title: null });
  expect(applyingNow(null, false)).toBeNull();
  // Kiln's uninstall takes its files and the mods it placed out of the game: Play waits, and says so
  expect(applyingNow(task("u", "runtime_uninstall", "running"), false)).toEqual({ what: "uninstall", title: "runtime_uninstall u" });
  expect([applyingWord({ what: "profile" }), applyingWord({ what: "kiln" }), applyingWord({ what: "uninstall" })])
    .toEqual(["Applying profile…", "Installing Kiln…", "Removing Kiln…"]);
});

test("the task list feeds it: a task's start, progress and end as kiln://task brings them", () => {
  const t = task("t1", "library_add", "running");
  useTasks.getState().upsert(t);
  expect(applyingTask(useTasks.getState().tasks)?.id).toBe("t1");
  useTasks.getState().upsert({ ...t, progress: 0.5, message: "Copying" });
  expect(applyingTask(useTasks.getState().tasks)?.id).toBe("t1");
  useTasks.getState().upsert({ ...t, state: "failed", finished: "2026-11-18T20:01:00Z", error: { code: "io", message: "The disk is full." } });
  expect(applyingTask(useTasks.getState().tasks)).toBeNull();
});

test("a late task_list answer does not bring an ended task back to running (Play would wait for it forever)", () => {
  const t = task("t2", "runtime_install", "running");
  useTasks.getState().upsert({ ...t, state: "done", progress: 1, finished: "2026-11-18T20:01:00Z" });
  useTasks.getState().upsert(t); // the snapshot, taken while it ran
  expect(useTasks.getState().tasks.t2.state).toBe("done");
  expect(applyingTask(useTasks.getState().tasks)).toBeNull();
});

test("a guarded activation marks its game while it runs and lets go when it ends or fails; Play's own start does not", async () => {
  let finish!: (v: string) => void;
  const load = oneActivation("g", () => new Promise<string>((ok) => { finish = ok; }));
  expect(useDirectActivations.getState().games).toEqual({ g: true });
  finish("done");
  await load;
  expect(useDirectActivations.getState().games).toEqual({});

  await expect(oneActivation("g", () => Promise.reject(new Error("no")))).rejects.toThrow("no");
  expect(useDirectActivations.getState().games).toEqual({});

  let started!: (v: string) => void;
  const play = oneActivation("g", () => new Promise<string>((ok) => { started = ok; }), { play: true });
  expect(useDirectActivations.getState().games).toEqual({});
  // a Load while Play's start runs is still refused, as before
  await expect(oneActivation("g", async () => "x")).rejects.toMatchObject({ code: "busy" });
  expect(useDirectActivations.getState().games).toEqual({});
  started("ok");
  await play;
});
