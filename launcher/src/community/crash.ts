// The crash notice (WIREFRAMES D10, K9): a banner on Play (a line on Deck Home), never a popup, after the game closed
// while Kiln wrote a crash report during that run. It follows the game's own events (kiln://game started and
// stopped); a run whose start the launcher did not see gets no notice. The notice stays until the game starts again or
// the player dismisses it. It does not need the Discord features: with the Community page hidden it shows without
// "Send to Kiln support".
import { create } from "zustand";
import * as api from "../api/commands";
import { t } from "../i18n";

export interface CrashNotice {
  gameId: string;
  /** When the game closed and when that run started (ms). */
  closedAt: number;
  startedAt: number;
  /** The crash report's file name (kiln/crash-reports/). */
  report: string;
}

interface CrashState {
  started: Record<string, number>;
  notice: Record<string, CrashNotice | undefined>;
  dismiss: (gameId: string) => void;
}

export const useCrash = create<CrashState>((set) => ({
  started: {},
  notice: {},
  dismiss: (gameId) => set((s) => ({ notice: { ...s.notice, [gameId]: undefined } })),
}));

/** A crash report counts for a run when it was written after the run started (with a little slack for the clocks). */
export function crashOfRun(reports: api.CrashReport[], startedAt: number, closedAt: number): api.CrashReport | null {
  const slack = 5_000;
  const hits = reports.filter((r) => { const at = Date.parse(r.created); return Number.isFinite(at) && at >= startedAt - slack && at <= closedAt + slack; });
  return hits.sort((a, b) => Date.parse(b.created) - Date.parse(a.created))[0] ?? null;
}

let listening = false;
/** Starts following the game's events (once). */
export function watchCrashes() {
  if (listening) return;
  listening = true;
  void api.onKilnEvent("kiln://game", (e) => {
    const id = e.gameId;
    if (e.kind === "started") {
      // the process's own start when the status has it (a start made outside the launcher too)
      const at = e.status?.started ? Date.parse(e.status.started) : NaN;
      useCrash.setState((s) => ({ started: { ...s.started, [id]: Number.isFinite(at) ? at : Date.now() }, notice: { ...s.notice, [id]: undefined } }));
      return;
    }
    if (e.kind !== "stopped") return;
    const startedAt = useCrash.getState().started[id];
    if (!startedAt) return;
    const closedAt = Date.now();
    useCrash.setState((s) => { const started = { ...s.started }; delete started[id]; return { started }; });
    api.logsCrashReports({ game: id }).then((list) => {
      const hit = crashOfRun(list, startedAt, closedAt);
      if (hit) useCrash.setState((s) => ({ notice: { ...s.notice, [id]: { gameId: id, closedAt, startedAt, report: hit.name } } }));
    }).catch(() => {});
  });
}

export function useCrashNotice(gameId: string | null | undefined): CrashNotice | null {
  watchCrashes();
  return useCrash((s) => (gameId ? s.notice[gameId] ?? null : null));
}

/** "4 minutes", "30 seconds", "2 hours" (en.json community.banner.after*). */
export function afterWords(ms: number): string {
  const s = Math.max(1, Math.round(ms / 1000));
  if (s < 90) return t("community.banner.afterSeconds", { count: s });
  const m = Math.round(s / 60);
  if (m < 90) return t("community.banner.afterMinutes", { count: m });
  return t("community.banner.afterHours", { count: Math.round(m / 60) });
}

/** "It closed at 22:10, 4 minutes after it started. Kiln wrote a crash report." */
export function crashLine(n: CrashNotice): string {
  const time = new Date(n.closedAt).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  return t("community.banner.crash", { time, after: afterWords(n.closedAt - n.startedAt) });
}
