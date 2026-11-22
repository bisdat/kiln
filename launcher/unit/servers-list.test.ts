// Servers revamp, the list (2026-11-22): the update rule (a server's Kiln is older than the newest server bundle of
// runtime_list: src/pages/servers/kilnUpdate.ts), the launcher-side activity log behind "Recent" (activityLog.ts: fed
// by runFlow when a flow ends, the last 50 kept in localStorage, a sentence per entry) and the list's sentence.
import { QueryClient } from "@tanstack/react-query";
import { beforeEach, expect, test } from "bun:test";
import type { RuntimeBundle, ServerStatus, Task } from "../src/api/commands";
import { t } from "../src/i18n";
import { compareKiln, kilnUpdateFor, newestServerBundle, shortKiln } from "../src/pages/servers/kilnUpdate";
import {
  ACTIVITY_KEY, ACTIVITY_MAX, activitySentence, entryFromFlow, fmtDuration, readActivity, reloadActivity, stoppedSince, useActivity, type ActivityEntry,
} from "../src/pages/servers/activityLog";
import { runFlow, useServerFlows, type Flow, type StepSpec } from "../src/pages/servers/serverActions";
import { listSentence } from "../src/pages/servers/listSay";
import type { ServerRow } from "../src/pages/servers/serverListCells";
import { useTasks } from "../src/ui/TaskProgress";

const bundle = (version: string, sides: ("server" | "client")[] = ["server", "client"]): RuntimeBundle =>
  ({ version, kiln: version.split("+")[0], file: `runtime/${version}.zip`, sha256: "0".repeat(64), created: null, sides, components: [], parts: [] });

// ---------------------------------------------------------------- the update rule

test("versions compare as semver with numeric pre-release parts, then the build date", () => {
  expect(compareKiln("0.2.0-dev.30.2", "0.2.0-dev.30.4")).toBe(-1);
  expect(compareKiln("0.2.0-dev.30.10", "0.2.0-dev.30.4")).toBe(1);
  expect(compareKiln("0.2.0-dev+20261104.0f1ffa6", "0.2.0-dev.30.2")).toBe(-1);
  expect(compareKiln("0.1.9+20261101.a41c2d0", "0.2.0-dev+20261104.0f1ffa6")).toBe(-1);
  expect(compareKiln("0.2.0", "0.2.0-dev.30.4")).toBe(1);
  expect(compareKiln("0.2.0-dev+20261104.0f1ffa6", "0.2.0-dev+20261105.1a2b3c4")).toBe(-1);
  expect(compareKiln("0.2.0-dev.30.4", "0.2.0-dev.30.4")).toBe(0);
  expect(compareKiln("unknown", "0.2.0-dev.30.4")).toBeNull();
});

test("a server has an update when its Kiln is older than the newest server bundle, never when none, unknown or remote", () => {
  const list = [bundle("0.2.0-dev.30.4"), bundle("0.2.0-dev.30.2"), bundle("0.2.0-dev+20261104.0f1ffa6")];
  expect(kilnUpdateFor("0.2.0-dev.30.2", list)).toBe("0.2.0-dev.30.4");
  expect(kilnUpdateFor("0.2.0-dev+20261104.0f1ffa6", list)).toBe("0.2.0-dev.30.4");
  // a version the launcher no longer keeps still compares
  expect(kilnUpdateFor("0.2.0-dev.29.18", list)).toBe("0.2.0-dev.30.4");
  expect(kilnUpdateFor("0.2.0-dev.30.4", list)).toBeNull();
  expect(kilnUpdateFor("0.2.0-dev.30.9", list)).toBeNull();
  expect(kilnUpdateFor(null, list)).toBeNull();
  expect(kilnUpdateFor("unknown", list)).toBeNull();
  expect(kilnUpdateFor("0.2.0-dev.30.2", list, true)).toBeNull();
  expect(kilnUpdateFor("0.2.0-dev.30.2", [])).toBeNull();
  expect(kilnUpdateFor("0.2.0-dev.30.2", undefined)).toBeNull();
});

test("the newest bundle is the newest with the server side, whatever the list's order", () => {
  const list = [bundle("0.2.0-dev.30.2"), bundle("0.2.0-dev.30.9", ["client"]), bundle("0.2.0-dev.30.4")];
  expect(newestServerBundle(list)?.version).toBe("0.2.0-dev.30.4");
  expect(kilnUpdateFor("0.2.0-dev.30.2", list)).toBe("0.2.0-dev.30.4");
  expect(newestServerBundle([bundle("0.2.0", ["client"])])).toBeNull();
  // the same version and day: runtime_list's order (newest first) decides
  const sameDay = [bundle("0.2.0-dev+20261104.bbbbbbb"), bundle("0.2.0-dev+20261104.aaaaaaa")];
  expect(kilnUpdateFor("0.2.0-dev+20261104.aaaaaaa", sameDay)).toBe("0.2.0-dev+20261104.bbbbbbb");
  expect(shortKiln("0.2.0-dev+20261104.0f1ffa6")).toBe("0.2.0-dev");
});

// ---------------------------------------------------------------- the activity log

class FakeStorage {
  data = new Map<string, string>();
  getItem(k: string) { return this.data.get(k) ?? null; }
  setItem(k: string, v: string) { this.data.set(k, v); }
  removeItem(k: string) { this.data.delete(k); }
}
const g = globalThis as unknown as { localStorage?: FakeStorage };

const flow = (p: Partial<Flow>): Flow => ({ seq: 1, kind: "start", serverId: "hollow-lab", title: "Starting Hollow Lab", steps: [], state: "done", startedAt: 1_000, endedAt: 42_000, ...p });
const done = (key: string, result?: unknown) => ({ key, label: key, state: "done" as const, progress: 1, lines: [], result });

beforeEach(() => {
  g.localStorage = new FakeStorage();
  reloadActivity();
  useTasks.setState({ tasks: {}, order: [] });
  useServerFlows.setState({ flows: {}, specs: {} });
});

test("an ended flow becomes an entry and a sentence: the time taken, the Kiln it runs, why it failed", () => {
  const started = entryFromFlow(flow({ steps: [done("start", { kiln: "0.2.0-dev.30.4" } as ServerStatus)] }), "Hollow Lab")!;
  expect(started).toMatchObject({ serverId: "hollow-lab", name: "Hollow Lab", kind: "start", state: "done", ms: 41_000, kiln: "0.2.0-dev.30.4", steps: ["start"] });
  expect(activitySentence(started, t)).toBe("Started in 41 s, Kiln 0.2.0-dev.30.4.");
  const stopped = entryFromFlow(flow({ kind: "stop", steps: [done("stop")], startedAt: 0, endedAt: 900 }))!;
  expect(activitySentence(stopped, t)).toBe("Stopped by you. Saved in 0.9 s.");
  const updated = entryFromFlow(flow({ kind: "deploy", meta: { from: "0.2.0-dev.30.2" }, startedAt: 0, endedAt: 64_000,
    steps: [done("stop"), done("deploy", { runtime: { version: "0.2.0-dev.30.4" } }), done("start", { kiln: "0.2.0-dev.30.4" })] }))!;
  expect(activitySentence(updated, t)).toBe("Kiln updated from 0.2.0-dev.30.2 to 0.2.0-dev.30.4 (stop, install, start: 1 min 4 s).");
  const failed = entryFromFlow(flow({ kind: "create", state: "failed", error: { code: "io", message: "steamcmd: Disk write failure" }, startedAt: 0, endedAt: 12_000,
    steps: [{ key: "create", label: "create", state: "failed", progress: null, lines: [] }, { key: "deploy", label: "deploy", state: "skipped", progress: null, lines: [] }] }))!;
  expect(failed.steps).toEqual(["create"]);
  expect(activitySentence(failed, t)).toBe("Install failed after 12 s: steamcmd: Disk write failure");
  const created = entryFromFlow(flow({ kind: "create", startedAt: 0, endedAt: 72_000,
    steps: [done("create"), done("deploy", { runtime: { version: "0.2.0-dev.30.4" } }), done("start", { kiln: "0.2.0-dev.30.4" })] }))!;
  expect(activitySentence(created, t)).toBe("Installed with Kiln 0.2.0-dev.30.4 and started in 1 min 12 s.");
  expect(entryFromFlow(flow({ state: "running" }))).toBeNull();
});

test("durations read like the mockup's", () => {
  expect(fmtDuration(900, t)).toBe("0.9 s");
  expect(fmtDuration(41_200, t)).toBe("41 s");
  expect(fmtDuration(64_000, t)).toBe("1 min 4 s");
  expect(fmtDuration(8_040_000, t)).toBe("2 h 14 min");
  expect(fmtDuration(443_000_000, t)).toBe("5 d 3 h");
});

test("runFlow writes an entry when a flow ends, with the server's name from the list; the log keeps the newest 50", async () => {
  const qc = new QueryClient();
  qc.setQueryData(["server_list"], [{ id: "hollow-lab", name: "Hollow Lab" }]);
  let n = 0;
  const step: StepSpec = {
    key: "start", label: "Start the server",
    run: async () => {
      const id = `start-${++n}`;
      const task: Task = { id, kind: "server_start", title: "start", state: "running", progress: 0, message: null, started: "x", finished: null };
      useTasks.getState().upsert(task);
      setTimeout(() => useTasks.getState().upsert({ ...task, state: "done", progress: 1, finished: "x", result: { kiln: "0.2.0-dev.30.2" } }), 0);
      return id;
    },
  };
  expect(await runFlow(qc, "hollow-lab", { kind: "start", title: "Starting Hollow Lab", steps: [step], done: "Server running" })).toBe(true);
  const [e] = useActivity.getState().entries;
  expect(e).toMatchObject({ serverId: "hollow-lab", name: "Hollow Lab", kind: "start", state: "done", kiln: "0.2.0-dev.30.2" });
  // kept in localStorage, newest first
  expect(JSON.parse(g.localStorage!.getItem(ACTIVITY_KEY)!)[0].kind).toBe("start");
  const t0 = Date.now() + 1000;
  for (let i = 0; i < ACTIVITY_MAX + 5; i++) useActivity.getState().add({ at: t0 + i, serverId: "x", kind: "backup", state: "done", ms: 1, steps: ["backup"] });
  expect(useActivity.getState().entries).toHaveLength(ACTIVITY_MAX);
  expect(useActivity.getState().entries[0].at).toBe(t0 + ACTIVITY_MAX + 4);
  expect(useActivity.getState().entries.some((x) => x.kind === "start")).toBe(false);
  expect(readActivity()).toHaveLength(ACTIVITY_MAX);
  // what another window wrote, or junk, reads back safely
  g.localStorage!.setItem(ACTIVITY_KEY, "{not json");
  expect(readActivity()).toEqual([]);
});

test("a flow another one took over (Stop during a start) writes no entry of its own", async () => {
  const qc = new QueryClient();
  let release = () => {};
  const held: StepSpec = {
    key: "start", label: "start", cancel: "stopWaiting",
    run: async () => {
      const task: Task = { id: "held-start", kind: "server_start", title: "start", state: "running", progress: 0, message: null, started: "x", finished: null };
      useTasks.getState().upsert(task);
      new Promise<void>((r) => { release = r; }).then(() => useTasks.getState().upsert({ ...task, state: "done", finished: "x" }));
      return task.id;
    },
  };
  const quick = (id: string): StepSpec => ({ key: "stop", label: "stop", run: async () => {
    const task: Task = { id, kind: "server_stop", title: "stop", state: "running", progress: 0, message: null, started: "x", finished: null };
    useTasks.getState().upsert(task);
    setTimeout(() => useTasks.getState().upsert({ ...task, state: "done", finished: "x" }), 0);
    return id;
  } });
  const starting = runFlow(qc, "lab", { kind: "start", title: "Starting", steps: [held], done: "Running" });
  await new Promise((r) => setTimeout(r, 0));
  await runFlow(qc, "lab", { kind: "stop", title: "Stopping", steps: [quick("stop-1")], done: "Stopped" }, { supersede: true });
  release();
  await starting;
  expect(useActivity.getState().entries.map((e) => e.kind)).toEqual(["stop"]);
});

test("a stopped server's time comes from its last stop, when nothing started it since", () => {
  const e = (kind: ActivityEntry["kind"], at: number, steps: string[] = [kind]): ActivityEntry => ({ at, serverId: "a", kind, state: "done", ms: 1, steps });
  expect(stoppedSince([e("stop", 30), e("start", 20)], "a")).toBe(30);
  expect(stoppedSince([e("backup", 40), e("stop", 30)], "a")).toBe(30);
  expect(stoppedSince([e("start", 40), e("stop", 30)], "a")).toBeNull();
  expect(stoppedSince([e("deploy", 40, ["stop", "deploy", "start"]), e("stop", 30)], "a")).toBeNull();
  expect(stoppedSince([], "a")).toBeNull();
});

// ---------------------------------------------------------------- the list's sentence

const row = (name: string, p: Partial<ServerRow> & { state?: ServerStatus["state"]; players?: number } = {}): ServerRow => ({
  srv: { id: name.toLowerCase().replace(/ /g, "-"), name, kind: "local", dir: "/srv", port: 1, queryPort: 2, wine: null, policy: "any", source: "kilnctl", remote: null },
  st: { id: name, state: p.state ?? "stopped", pid: null, uptimeS: null, players: p.players ?? null, kiln: null, boot: null, endpoint: null },
  flow: undefined, remote: false, update: null, kiln: null, maxPlayers: null, rates: [], ...p,
});

test("the header says what runs, with how many players, and which server has a Kiln update", () => {
  const story = [row("Ember Isles", { state: "running", players: 3 }), row("Hollow Lab", { update: "0.2.0-dev.30.4" }), row("Frostwood", { state: "running", players: 0 })];
  expect(listSentence(story, t)).toBe("3 servers. Ember Isles is running with 3 players; Hollow Lab has a Kiln update.");
  expect(listSentence([row("Lab server", { state: "running", players: 0 })], t)).toBe("1 server. Lab server is running with nobody on it.");
  expect(listSentence([row("A", { state: "running", players: 2 }), row("B", { state: "running", players: 1 })], t)).toBe("2 servers. 2 are running with 3 players in all.");
  expect(listSentence([row("A"), row("B")], t)).toBe("2 servers. None is running.");
  const busy = flow({ serverId: "b", state: "running", kind: "deploy" });
  expect(listSentence([row("A", { state: "running", players: 1 }), row("B", { flow: busy, update: "x" })], t))
    .toBe("2 servers. B is updating Kiln; A is running with 1 player.");
});
