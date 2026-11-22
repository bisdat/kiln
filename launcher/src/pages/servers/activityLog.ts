// The Servers list's "Recent": the last actions on any server, newest first ("Started in 41 s, Kiln 0.2.0-dev.30.4.").
// There is no activity log in the backend yet (kiln_manage keeps none: the servers-revamp mockups' README lists it as a
// backend need), so this is a small launcher-side log: runFlow (serverActions.ts) hands every flow it ends here, from
// the list, a server's page or the new-server wizard, and the list's own Remove adds a line. It only knows what this
// launcher did on this PC (not kilnctl, another launcher or a server's own schedule). The last MAX entries are kept in
// localStorage; an entry is data (the kind, the time taken, the Kiln version), turned into a sentence when it shows, so
// it follows the language.
// Imports from serverActions.ts are types only: serverActions imports this module.
import type { QueryClient } from "@tanstack/react-query";
import { create } from "zustand";
import type { InstallResult, ServerStatus } from "../../api/commands";
import { formatLocale, type TFunction } from "../../i18n";
import type { Flow, FlowKind } from "./serverActions";

export const ACTIVITY_KEY = "kiln.servers.activity";
export const ACTIVITY_MAX = 50;

export type ActivityKind = FlowKind | "remove";

export interface ActivityEntry {
  /** ms since the epoch, when it ended */
  at: number;
  serverId: string;
  /** the server's name then (a removed server keeps it) */
  name?: string;
  kind: ActivityKind;
  state: "done" | "failed" | "canceled";
  /** how long it took (ms) */
  ms: number;
  /** the keys of the steps that ran ("stop", "deploy", "start") */
  steps: string[];
  /** the Kiln version after it: a start's running Kiln, an install's version */
  kiln?: string | null;
  /** an update's version before it */
  from?: string | null;
  /** why it failed */
  reason?: string;
}

function storage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

/** The entries kept in localStorage (none when it is missing or unreadable), newest first. */
export function readActivity(): ActivityEntry[] {
  try {
    const v = JSON.parse(storage()?.getItem(ACTIVITY_KEY) ?? "[]");
    return Array.isArray(v) ? v.filter((e) => e && typeof e.at === "number" && typeof e.serverId === "string" && typeof e.kind === "string").slice(0, ACTIVITY_MAX) : [];
  } catch {
    return [];
  }
}

function save(entries: ActivityEntry[]) {
  try {
    storage()?.setItem(ACTIVITY_KEY, JSON.stringify(entries));
  } catch {
    /* the log lasts for this window only */
  }
}

interface ActivityStore {
  entries: ActivityEntry[];
  add: (e: ActivityEntry) => void;
}

export const useActivity = create<ActivityStore>((set, get) => ({
  entries: readActivity(),
  add: (e) => {
    const entries = [e, ...get().entries].sort((a, b) => b.at - a.at).slice(0, ACTIVITY_MAX);
    save(entries);
    set({ entries });
  },
}));

/** Reads localStorage again (tests; another window's writes). */
export function reloadActivity() {
  useActivity.setState({ entries: readActivity() });
}

/** The entry of a flow that ended (null while it runs). */
export function entryFromFlow(f: Flow, name?: string): ActivityEntry | null {
  if (f.state === "running") return null;
  const ran = f.steps.filter((s) => s.state === "done" || s.state === "failed" || s.state === "canceled");
  const result = (key: string) => f.steps.find((s) => s.key === key && s.state === "done")?.result;
  const started = (result("start") ?? result("restart")) as ServerStatus | null | undefined;
  const installed = result("deploy") as InstallResult | null | undefined;
  const kiln = installed?.runtime?.version ?? started?.kiln ?? null;
  return {
    at: f.endedAt ?? Date.now(),
    serverId: f.serverId,
    ...(name ? { name } : {}),
    kind: f.kind,
    state: f.state,
    ms: Math.max(0, (f.endedAt ?? Date.now()) - f.startedAt),
    steps: ran.map((s) => s.key),
    ...(kiln ? { kiln } : {}),
    ...(f.meta?.from ? { from: f.meta.from } : {}),
    ...(f.state === "failed" ? { reason: f.error?.message ?? "" } : {}),
  };
}

/** Called by runFlow when one of its flows ends (not one a later flow took over: that one's end is the later flow's). */
export function recordFlowEnd(qc: QueryClient, f: Flow | undefined) {
  if (!f || f.superseded) return;
  // sk.list in serverApi.ts (not imported: serverApi imports serverActions, which imports this module)
  const name = qc.getQueryData<{ id: string; name: string }[]>(["server_list"])?.find((s) => s.id === f.serverId)?.name;
  const e = entryFromFlow(f, name);
  if (e) useActivity.getState().add(e);
}

/** "0.9 s", "41 s", "1 min 4 s", "2 h 14 min", "5 d 3 h". */
export function fmtDuration(ms: number, t: TFunction): string {
  const s = Math.max(0, ms) / 1000;
  if (s < 10) return t("servers.head.durS", { s: (Math.floor(s * 10) / 10).toLocaleString(formatLocale(), { maximumFractionDigits: 1 }) });
  const whole = Math.floor(s);
  if (whole < 60) return t("servers.head.durS", { s: whole });
  if (whole < 3600) return t("servers.head.durM", { m: Math.floor(whole / 60), s: whole % 60 });
  if (whole < 86400) return t("servers.head.durH", { h: Math.floor(whole / 3600), m: Math.floor((whole % 3600) / 60) });
  return t("servers.list.durD", { d: Math.floor(whole / 86400), h: Math.floor((whole % 86400) / 3600) });
}

/** "01:12" today, else the day ("Nov 21"). */
export function fmtClock(at: number, now = Date.now()): string {
  const d = new Date(at);
  const today = new Date(now).toDateString() === d.toDateString();
  return today ? d.toLocaleTimeString(formatLocale(), { hour: "2-digit", minute: "2-digit" }) : d.toLocaleDateString(formatLocale(), { month: "short", day: "numeric" });
}

/** What the action was, as the subject of "… failed" ("Start", "Kiln install"). */
function what(kind: ActivityKind, t: TFunction): string {
  switch (kind) {
    case "start": return t("servers.recent.what.start");
    case "stop": return t("servers.recent.what.stop");
    case "restart": return t("servers.recent.what.restart");
    case "update": return t("servers.recent.what.update");
    case "deploy": return t("servers.recent.what.deploy");
    case "create": return t("servers.recent.what.create");
    case "backup": return t("servers.recent.what.backup");
    case "remove": return t("servers.recent.what.remove");
  }
}

/** A step's short word in "(stop, install, start: 1 min 4 s)". */
function stepWord(key: string, t: TFunction): string {
  switch (key) {
    case "stop": return t("servers.recent.step.stop");
    case "deploy": return t("servers.recent.step.deploy");
    case "start": return t("servers.recent.step.start");
    case "update": return t("servers.recent.step.update");
    case "create": return t("servers.recent.step.create");
    default: return key;
  }
}

/** The entry as one sentence: "Started in 41 s, Kiln 0.2.0-dev.30.4.", "Stopped by you. Saved in 0.9 s.". */
export function activitySentence(e: ActivityEntry, t: TFunction): string {
  const time = fmtDuration(e.ms, t);
  if (e.state === "failed") return t("servers.recent.failed", { what: what(e.kind, t), time, reason: e.reason || t("servers.recent.noReason") });
  if (e.state === "canceled") return t("servers.recent.canceled", { what: what(e.kind, t), time });
  const kiln = e.kiln ?? "";
  const steps = e.steps.map((k) => stepWord(k, t)).join(", ");
  switch (e.kind) {
    case "start": return kiln ? t("servers.recent.startedKiln", { time, kiln }) : t("servers.recent.started", { time });
    case "stop": return t("servers.recent.stopped", { time });
    case "restart": return t("servers.recent.restarted", { time });
    case "update": return t("servers.recent.gameUpdated", { time });
    case "deploy":
      if (e.from && kiln && e.from !== kiln) return t("servers.recent.kilnUpdated", { from: e.from, kiln, steps, time });
      return e.steps.length > 1 ? t("servers.recent.kilnInstalledSteps", { kiln, steps, time }) : t("servers.recent.kilnInstalled", { kiln, time });
    case "create": {
      const started = e.steps.includes("start");
      if (e.steps.includes("deploy") && kiln) return started ? t("servers.recent.createdKilnStarted", { kiln, time }) : t("servers.recent.createdKiln", { kiln, time });
      return started ? t("servers.recent.createdStarted", { time }) : t("servers.recent.created", { time });
    }
    case "backup": return t("servers.recent.backedUp", { time });
    case "remove": return t("servers.recent.removed");
  }
}

/** When a stopped server stopped, from the log: the time of its last stop when that is its last start, stop or restart. */
export function stoppedSince(entries: ActivityEntry[], serverId: string): number | null {
  const last = entries.find((e) => e.serverId === serverId && e.state === "done" && (e.kind === "start" || e.kind === "stop" || e.kind === "restart"
    || (e.kind === "deploy" && e.steps.includes("start")) || (e.kind === "create" && e.steps.includes("start"))));
  return last?.kind === "stop" ? last.at : null;
}
