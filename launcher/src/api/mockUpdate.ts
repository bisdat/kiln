// Mock answers for the self-update commands (update.rs), merged into ./mock.ts's handlers. Up to date by default;
// tests and the dev console change it through window.__kilnUpdateMock:
//   offer("0.2.0-dev.43", "notes")        the next check finds this version (and the notice opens)
//   justUpdated("0.2.0-dev.42")           as after a restart into an update this launcher installed (`?justUpdated=<from>`:
//                                         from the first status on, as the backend's start says it)
//   failNextInstall([{source, step, message}])  the next download fails at these sources
//   set({...})                            any part of the status, then kiln://update
//   calls()                               the update_* commands called so far (the Nexus build's UI calls none)
//   beta(3, "0.2.0-dev.43")               (dev.29) this build is test build 3 of that version (with a "before beta" copy)
//   channelOffer("dev", "0.2.0-dev.42")   (dev.29) what a switch to that channel installs (null: it has nothing else)
//   switches()                            (dev.29) the update_switch calls so far ({channel, restore})
// `shippedRuntime` is the mock's bundle 0.2.0-dev+20261104.0f1ffa6 (the runtime this launcher ships, 30.5). update_restart
// refuses while `gameBusy` says a game runs or starts (selfupdate.rs restart_refusal).
// `?dist=nexus` (the Nexus Mods build): like the backend without the self-update feature (commands/update.rs): the
// status is "disabled", update_check changes nothing, update_install and update_restart reject with `unsupported`.
import { parseBuild } from "./channel";
import type { Distribution, KilnError, Task, UpdateChannel, UpdateFailure, UpdateStatus } from "./commands";
import { NEXUS_PAGE } from "./distribution";

type Emit = (name: string, payload: unknown) => void;
type Args = Record<string, unknown>;

/** The runtime the mock launcher ships (a bundle of the mock's runtime list). */
export const MOCK_SHIPPED_RUNTIME = "0.2.0-dev+20261104.0f1ffa6";

/** `channelSetting`: the mock settings' `updateChannel` (the backend reads settings.json for the channel each time).
 *  `gameBusy`: why the restart waits (a game runs or starts), null when it does not. */
export function updateMockHandlers(emit: Emit, distribution: Distribution = "direct", channelSetting: () => string | null | undefined = () => null,
  gameBusy: () => KilnError | null = () => null): Record<string, (a: Args) => unknown> {
  const now = () => new Date().toISOString();
  const nexus = distribution === "nexus";
  const noSelfUpdate = "This build comes from Nexus Mods and does not update itself: new versions are on the Kiln page there.";
  let status: UpdateStatus = nexus ? {
    current: "0.2.0-dev.42", distribution: "nexus", channel: "dev", auto: false, install: { kind: "appimage", inPlace: false, reason: noSelfUpdate },
    page: NEXUS_PAGE, phase: "disabled", checked: null, available: null, failures: [], progress: null,
    ready: null, skipped: null, justUpdated: null, shippedRuntime: null, taskId: null, error: null, buildChannel: "dev", beta: null, switching: null, beforeBeta: null,
  } : {
    current: "0.2.0-dev.42", distribution: "direct", channel: "dev", auto: true, install: { kind: "appimage", inPlace: true, reason: null },
    page: "https://frostdev.io/kiln", phase: "upToDate", checked: now(), available: null, failures: [], progress: null,
    ready: null, skipped: null, justUpdated: null, shippedRuntime: MOCK_SHIPPED_RUNTIME, taskId: null, error: null, buildChannel: "dev", beta: null, switching: null, beforeBeta: null,
  };
  // what a switch to each channel installs (update_switch); the release line has the build before this one
  const fromQuery = typeof location !== "undefined" && !nexus ? new URLSearchParams(location.search).get("justUpdated") : null;
  if (fromQuery) status.justUpdated = { from: fromQuery, to: status.current, at: now(), source: "frostdev.io", notes: null };
  const channelOffers: Partial<Record<UpdateChannel, string | null>> = { dev: "0.2.0-dev.41", beta: "0.2.0-dev.43+beta.1" };
  const switchCalls: { channel: UpdateChannel; restore: boolean }[] = [];
  let restoreOnRestart = false;
  // the channel the backend reports: the setting, else the build's own (feed.rs Channel::effective)
  const known = (c: unknown): c is UpdateChannel => c === "stable" || c === "dev" || c === "beta";
  const withChannel = () => { if (!nexus) { const c = channelSetting(); status.channel = known(c) ? c : status.buildChannel ?? "dev"; } return status; };
  const calls: string[] = [];
  const refuse = (): never => { throw { code: "unsupported", message: noSelfUpdate, detail: NEXUS_PAGE } satisfies KilnError; };
  let offer: UpdateStatus["available"] = null;
  let failNext: UpdateFailure[] | null = null;
  let seq = 0;
  const send = () => emit("kiln://update", structuredClone(withChannel()));
  const put = (p: Partial<UpdateStatus>) => { status = { ...status, ...p }; send(); return structuredClone(status); };
  const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

  const check = async () => {
    if (status.phase === "downloading" || status.phase === "ready") return status;
    put({ phase: "checking" });
    await sleep(500);
    if (!offer) return put({ phase: "upToDate", available: null, checked: now(), failures: [] });
    return put({ phase: status.skipped === offer.version ? "skipped" : "available", available: offer, checked: now(), failures: [] });
  };

  const install = (switchTo: UpdateChannel | null = null) => {
    if (!status.install.inPlace) throw { code: "unsupported", message: status.install.reason ?? "This install cannot update itself." } satisfies KilnError;
    if (status.phase === "ready") throw { code: "conflict", message: "The update is ready: restart the launcher to use it." } satisfies KilnError;
    if (status.phase === "downloading") throw { code: "busy", message: "An update check or download is already running." } satisfies KilnError;
    let version = status.available?.version ?? "0.2.0-dev.43";
    if (switchTo) {
      const v = channelOffers[switchTo];
      if (!v) throw { code: "not_found", message: "That channel has no other build for this launcher." } satisfies KilnError;
      version = v;
    }
    const id = `t-update-${++seq}`;
    const total = 96_000_000;
    const task: Task = switchTo
      ? { id, kind: "update_switch", title: switchTo === "beta" ? "Switch to the beta" : "Switch back to the release", state: "running", progress: 0, message: "Looking for the build", started: now(), finished: null }
      : { id, kind: "update_install", title: "Update the launcher", state: "running", progress: 0, message: "Looking for the update", started: now(), finished: null };
    if (switchTo) status.switching = switchTo;
    const failures = failNext;
    failNext = null;
    put({ phase: "downloading", taskId: id, progress: null, error: null, failures: [] });
    emit("kiln://task", task);
    let received = 0;
    const tick = () => {
      received = Math.min(total, received + 9_600_000);
      const source = failures?.length ? "GitHub" : "frostdev.io";
      put({ progress: { received, total, source } });
      emit("kiln://task", { ...task, progress: received / total, message: `Downloading from ${source}: ${(received / 1048576).toFixed(1)} of ${(total / 1048576).toFixed(1)} MB` });
      if (received < total) { setTimeout(tick, 160); return; }
      if (failures?.length && failures.length > 1) {
        const error: KilnError = { code: "refused", message: "The update could not be downloaded and verified from any source.", detail: failures.map((f) => `${f.source}: ${f.message}`).join("; ") };
        put({ phase: status.available ? "available" : "upToDate", progress: null, failures, error, switching: null });
        emit("kiln://task", { ...task, state: "failed", message: error.message, finished: now(), error });
        return;
      }
      put({ phase: "ready", progress: null, failures: failures ?? [], ready: { version, source, installed: true } });
      emit("kiln://task", { ...task, state: "done", progress: 1, message: `Verified ${version} from ${source}`, finished: now(), result: status });
    };
    setTimeout(tick, 200);
    return id;
  };

  const api = {
    set: (p: Partial<UpdateStatus>) => put(p),
    offer: (version: string, notes: string | null = null, runtime: string | null = null) => {
      offer = { version, notes, date: now(), source: "frostdev.io", page: "https://frostdev.io/kiln", runtime };
      return check();
    },
    justUpdated: (from: string, notes: string | null = null) => put({
      justUpdated: { from, to: status.current, at: now(), source: "frostdev.io", notes }, phase: "upToDate", available: null, ready: null,
    }),
    failNextInstall: (f: UpdateFailure[]) => { failNext = f; },
    calls: () => [...calls],
    beta: (n: number, version = "0.2.0-dev.43") => put({
      current: version, buildChannel: "beta", beta: n,
      beforeBeta: { taken: now(), from: "0.2.0-dev.42", by: `${version}+beta.1`, files: ["profiles/default/meta.json", "settings.json"] },
    }),
    channelOffer: (channel: UpdateChannel, version: string | null) => { channelOffers[channel] = version; },
    switches: () => [...switchCalls],
  };
  if (typeof window !== "undefined") (window as unknown as { __kilnUpdateMock?: typeof api }).__kilnUpdateMock = api;

  const handlers: Record<string, (a: Args) => unknown> = nexus ? {
    update_status: () => structuredClone(status),
    update_check: () => structuredClone(status),
    update_install: () => refuse(),
    update_restart: () => refuse(),
    update_skip: () => structuredClone(status),
    update_ack: () => structuredClone(status),
    update_switch: () => refuse(),
  } : {
    update_status: () => withChannel(),
    update_check: () => check(),
    update_install: () => install(),
    update_switch: (a) => {
      const channel = a.channel as UpdateChannel;
      if (!known(channel)) throw { code: "invalid_argument", message: `no channel ${String(a.channel)}` } satisfies KilnError;
      switchCalls.push({ channel, restore: !!a.restore });
      restoreOnRestart = !!a.restore;
      status.restoreRequested = restoreOnRestart;
      return install(channel);
    },
    update_restart: () => {
      if (status.phase !== "ready" || !status.ready) throw { code: "conflict", message: "No update is ready." } satisfies KilnError;
      const busy = gameBusy();
      if (busy) throw busy;
      const to = status.ready.version;
      const from = status.beta ? `${status.current}+beta.${status.beta}` : status.current;
      const b = parseBuild(to);
      offer = null;
      // a restart into another channel's build: that build's own channel and beta number (the mock's settings keep the
      // channel the player chose; a restore put the "before beta" settings back, which the mock does not model)
      const build: Partial<UpdateStatus> = status.switching || b.beta !== null || status.beta
        ? { buildChannel: b.beta !== null ? "beta" : "dev", beta: b.beta, beforeBeta: b.beta !== null ? status.beforeBeta : (restoreOnRestart ? null : status.beforeBeta) }
        : {};
      put({ justUpdated: { from, to, at: now(), source: status.ready.source, notes: status.available?.notes ?? null }, current: b.version, phase: "upToDate", available: null, ready: null, taskId: null, switching: null, ...build });
      restoreOnRestart = false;
      status.restoreRequested = false;
      return null;
    },
    update_skip: (a) => {
      const v = (a.version as string | null) ?? null;
      return put({ skipped: v, phase: status.available ? (status.available.version === v ? "skipped" : "available") : status.phase });
    },
    update_ack: () => put({ justUpdated: null }),
  };
  // every update_* call is recorded (tests check the Nexus build's UI makes none)
  return Object.fromEntries(Object.entries(handlers).map(([name, h]) => [name, (a: Args) => { calls.push(name); return h(a); }]));
}
