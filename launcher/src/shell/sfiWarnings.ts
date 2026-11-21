// the warning before Play. Every start this launcher asks for (the shell's Play and its
// pilot light, the Play page, Play once, the Deck's Home, a modpack offer's Launch) goes through launchWithPreflight
// after its own steps (switch back, cook, the boot game-data step): launch_play runs the start up to its check (the
// prepare: with warnings it opens nothing and answers them as a preflight with a one-start token, consent_required;
// without, it starts as before); with warnings, the dialog (components/SfiWarningDialog.tsx) waits for
// the player, and only "Continue without these mods" confirms that exact token (launch_confirm). Nothing else
// acknowledges a warning: not Settings' folder notes, not an earlier activation notice dismissed, not a toast. The
// backend stays the authority: launch_play itself refuses a start whose warnings nobody acknowledged
// (kiln_manage::sfi_launch).
//
// Rules the backend sets and this follows:
// - Any confirm uses its token up, refused, busy or failed ones too: a later try prepares again (the next Play).
// - A confirm that finds the warnings changed answers consent_required with the new preflight in detail.preflight:
//   that is a new dialog, never a confirm of its own.
// - While the dialog waits the start goes on being timed (launch_begin's timeline stays); a consent_required between
//   two dialogs does not end it. Only the end of the whole start ends it: Cancel, a refusal, a failure (play.ts and
//   ModpackOffer call launch_end then, as before).
// - launch_cancel is not bound to a window: the token is 32 random bytes only the window that prepared it was given,
//   and a cancel can only drop a start, never open one. A window cancels only tokens it holds.
import { useEffect } from "react";
import { create } from "zustand";
import * as api from "../api/commands";
import type { GameId, KilnError, LaunchRequest, LaunchResult, Preflight, StartWarning } from "../api/commands";
import { t } from "../i18n";

/** One dialog that waits: the preflight to show, and how many dialogs this start showed before it (0: the first). */
export interface StartAsk {
  id: number;
  preflight: Preflight;
  round: number;
  answer: (go: boolean) => void;
}

/** The dialogs waiting, the oldest shown first, and how many dialog hosts are mounted (AppShell, DeckApp). */
export const useStartWarnings = create<{ asks: StartAsk[]; hosts: number }>(() => ({ asks: [], hosts: 0 }));

let seq = 0;

/** Shows the warnings and waits for the player: true for "Continue without these mods", false for Cancel, Escape,
 *  back, a click beside the dialog or the dialog going away. With no host mounted, false at once: a start nobody could
 *  look at is never confirmed. */
export function askStartWarnings(preflight: Preflight, round = 0): Promise<boolean> {
  if (useStartWarnings.getState().hosts <= 0) return Promise.resolve(false);
  return new Promise<boolean>((resolve) => {
    const id = ++seq;
    const answer = (go: boolean) => {
      useStartWarnings.setState((s) => ({ asks: s.asks.filter((a) => a.id !== id) }));
      resolve(go);
    };
    useStartWarnings.setState((s) => ({ asks: [...s.asks, { id, preflight, round, answer }] }));
  });
}

/** For a dialog host: counts it while mounted, and answers Cancel to every waiting dialog when the last one goes.
 *  The dialog to show now (null: none). */
export function useStartWarningHost(): StartAsk | null {
  useEffect(mountStartWarningHost, []);
  return useStartWarnings((s) => s.asks[0] ?? null);
}

/** Counts one host; the function it returns takes it away again (the last one going cancels every waiting dialog). */
export function mountStartWarningHost(): () => void {
  useStartWarnings.setState((s) => ({ hosts: s.hosts + 1 }));
  return () => {
    useStartWarnings.setState((s) => ({ hosts: s.hosts - 1 }));
    if (useStartWarnings.getState().hosts <= 0) useStartWarnings.getState().asks.forEach((a) => a.answer(false));
  };
}

/** The message of the error a canceled start ends with (no toast: the player chose it). */
export const START_CANCELED = "The start was canceled before the game was opened.";
export const canceledStart = (): KilnError => ({ code: "canceled", message: START_CANCELED });
export function isCanceledStart(e: unknown): boolean {
  const k = e as Partial<KilnError> | null;
  return !!k && k.code === "canceled" && k.message === START_CANCELED;
}

/** The new preflight a consent_required carries (launch_play's or launch_confirm's), or null. */
export function preflightOf(e: unknown): Preflight | null {
  const k = e as { code?: unknown; detail?: { preflight?: Preflight } } | null;
  const p = k?.code === "consent_required" ? k.detail?.preflight : undefined;
  return p && typeof p.token === "string" && Array.isArray(p.warnings) ? p : null;
}

/** A preflight of another game than the one asked for: refused, nothing confirmed. */
const otherGame = (): KilnError => ({ code: "refused", message: t("sfBackend.startWarning.otherGame") });

/** The three commands the check before Play uses (tests give their own). */
export interface StartIo {
  play: (gameId: GameId, req: LaunchRequest) => Promise<LaunchResult>;
  confirm: (gameId: GameId, token: string, req: LaunchRequest) => Promise<LaunchResult>;
  cancel: (gameId: GameId, token: string) => Promise<boolean>;
}
const apiIo: StartIo = { play: api.launchPlay, confirm: api.launchConfirm, cancel: api.launchCancel };

/** The start of `req` in `gameId` with the check before Play: launch_play (the prepare), the dialog when it answers
 *  warnings, confirm of exactly that token. A changed set of warnings (consent_required with a new preflight) is a new dialog. Cancel
 *  drops the token and rejects with canceledStart(); any other failure rejects as it came (its token cancelled, in
 *  case the confirm never reached the backend). `gameId` travels with the call: never a closure's game. */
export async function launchWithPreflight(gameId: GameId, req: LaunchRequest, ask: typeof askStartWarnings = askStartWarnings, io: StartIo = apiIo): Promise<LaunchResult> {
  // launch_play is the prepare: it runs the start up to the check and opens nothing while warnings wait (answering
  // them as a preflight); without warnings it starts at once, so a start with nothing to say costs one refresh, not two
  let p: Preflight;
  try {
    return await io.play(gameId, req);
  } catch (e) {
    const first = preflightOf(e);
    if (!first) throw e;
    p = first;
  }
  for (let round = 0; ; round++) {
    if (p.gameId !== gameId) {
      await io.cancel(p.gameId, p.token).catch(() => false);
      throw otherGame();
    }
    // every preflight is shown (a changed set too), never confirmed on its own
    const go = await ask(p, round).catch(() => false);
    if (!go) {
      await io.cancel(gameId, p.token).catch(() => false);
      throw canceledStart();
    }
    try {
      return await io.confirm(gameId, p.token, req);
    } catch (e) {
      const next = preflightOf(e);
      if (!next) {
        await io.cancel(gameId, p.token).catch(() => false);
        throw e;
      }
      p = next;
    }
  }
}

// ---------------------------------------------------------------- what the dialog says

/** The strongest thing the warnings say, for the dialog's lead: save (a save-sensitive package is off), check (Kiln
 *  could not check everything), or plain (mods do not run). */
export function strongest(warnings: readonly StartWarning[]): "save" | "check" | "plain" {
  if (warnings.some((w) => w.severity === "save-warning" || w.package?.saveSensitive)) return "save";
  if (warnings.some((w) => w.severity === "infrastructure-error")) return "check";
  return "plain";
}

/** A package as the dialog names it: "Quiver (ammo-slots 0.8.2)", "ammo-slots 0.8.2". */
export function packageLabel(id: string, version?: string | null, name?: string | null): string {
  const iv = version ? `${id} ${version}` : id;
  return name && name !== id ? `${name} (${iv})` : iv;
}

/** One warning as the dialog lists it: its heading, its lines (each in full), whether it is about save data. */
export interface WarningText {
  key: string;
  title: string;
  lines: string[];
  save: boolean;
}

export function warningText(w: StartWarning, i = 0): WarningText {
  const lines: string[] = [];
  const save = w.severity === "save-warning" || !!w.package?.saveSensitive;
  const key = `${i}:${w.kind}:${w.code}:${w.package?.id ?? w.previous?.id ?? ""}`;
  if (w.kind === "package-skipped" && w.package) {
    const p = w.package;
    for (const r of p.reasons.length ? p.reasons : [{ code: w.code, message: w.message }]) lines.push(r.message);
    const parts = [p.data && t("sfBackend.startWarning.partData"), p.lua && t("sfBackend.startWarning.partLua"), p.dlls.length && t("sfBackend.startWarning.partDlls", { list: p.dlls.join(", ") })].filter(Boolean) as string[];
    if (parts.length) lines.push(t("sfBackend.startWarning.parts", { list: parts.join(" · ") }));
    if (p.data) lines.push(t("sfBackend.startWarning.dataNotApplied"));
    if (w.causedBy?.length) lines.push(t("sfBackend.startWarning.chain", { chain: w.causedBy.join(" → ") }));
    lines.push(t("sfBackend.startWarning.folder", { root: p.root }));
    if (w.previous) lines.push(t("sfBackend.startWarning.wasActive", { launch: w.previous.fromLaunchId ?? t("sfBackend.startWarning.unknownStart") }));
    if (save) lines.push(p.saveSensitive || w.previous ? t("sfBackend.startWarning.saveLine") : t("sfBackend.startWarning.saveUnknown"));
    return { key, title: t("sfBackend.startWarning.skipped", { name: packageLabel(p.id, p.version) }), lines, save };
  }
  if (w.kind === "start-refused") {
    lines.push(w.message);
    if (w.packages?.length) lines.push(t("sfBackend.startWarning.notStarted", { list: w.packages.join(", ") }));
    return { key, title: t("sfBackend.startWarning.refused"), lines, save };
  }
  if (w.kind === "previously-active-off" && w.previous) {
    const p = w.previous;
    lines.push(t("sfBackend.startWarning.offNow"));
    lines.push(t("sfBackend.startWarning.wasActive", { launch: p.fromLaunchId ?? t("sfBackend.startWarning.unknownStart") }));
    lines.push(p.basis === "policy" ? t("sfBackend.startWarning.basisPolicy") : t("sfBackend.startWarning.basisUnknown"));
    lines.push(t("sfBackend.startWarning.saveLine"));
    return { key, title: t("sfBackend.startWarning.previous", { name: packageLabel(p.id, p.version, p.name) }), lines, save: true };
  }
  lines.push(w.message);
  if (w.causedBy?.length) lines.push(t("sfBackend.startWarning.about", { list: w.causedBy.join(", ") }));
  return { key, title: w.code === "inspector-unresolved" ? t("sfBackend.startWarning.inspector") : t("sfBackend.startWarning.planWarning"), lines, save };
}

/** An advisory as the dialog shows it, apart from the warnings (it claims no package is skipped). `i`: its place in the
 *  list, part of its key (two advisories may share a kind and a code). */
export function advisoryText(a: StartWarning, i = 0): { key: string; title: string; lines: string[] } {
  if (a.kind === "base-origin") return { key: `${i}:base:${a.code}`, title: t("sfBackend.startWarning.baseTitle"), lines: [a.message, t("sfBackend.startWarning.baseRestore"), t("sfBackend.startWarning.baseVerify")] };
  if (a.code === "keybinds-unsupported") {
    const name = a.packages?.[0] ?? "";
    return { key: `${i}:${a.kind}:${a.code}:${name}`, title: t("sfBackend.startWarning.keybindsTitle"), lines: [name ? t("sfBackend.startWarning.keybinds", { name }) : a.message] };
  }
  return { key: `${i}:${a.kind}:${a.code}`, title: a.kind === "cost" ? t("sfBackend.startWarning.costTitle") : t("sfBackend.startWarning.noteTitle"), lines: [a.message] };
}
