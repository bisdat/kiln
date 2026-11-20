// (dev.29.18) The upscaler behind the game's FSR 3 option, as the launcher says it (Settings, Graphics; the Deck's
// Settings): what runs and why the choice does not, from the game's kiln/state/graphics.json (GameStatus.graphicsState:
// "Now" while the game that wrote it runs, "Last game" after), else what Kiln will do from the choice, the files in
// kiln/vendor/ and the tools in the game folder. The reason texts are the game's own (runtime/locales/en.json
// graphics.upscaler.reason.*, the same English in src/i18n/en.json; unit/upscaler-status.test.ts keeps them one table)
// and docs/kiln/graphics.md "What runs now, and why not" lists them.
import { UPSCALER_REASONS, type AmdUpscalerMode, type GraphicsNow, type GraphicsStepAside, type Json, type UpscalerChoice, type UpscalerReason } from "../api/commands";
import type { VendorComponent } from "../api/vendor";
import { t } from "../i18n";

export const UPSCALER_CHOICES: UpscalerChoice[] = ["game", "amd", "xess"];
export const AMD_MODES: AmdUpscalerMode[] = ["auto", "fsr3", "fsr4"];

/** kiln.json `graphics.upscaler` from the document's values (the game's own for anything else). */
export function choiceOf(values: Json | undefined): UpscalerChoice {
  const v = (values as { graphics?: { upscaler?: unknown } } | undefined)?.graphics?.upscaler;
  return v === "amd" || v === "xess" ? v : "game";
}

/** kiln.json `graphics.amdUpscaler` (auto for anything else). */
export function amdModeOf(values: Json | undefined): AmdUpscalerMode {
  const v = (values as { graphics?: { amdUpscaler?: unknown } } | undefined)?.graphics?.amdUpscaler;
  return v === "fsr3" || v === "fsr4" ? v : "auto";
}

/** The vendor folder of a choice (the manifest's `vendor`), null for the game's own. */
export const vendorOf = (c: UpscalerChoice): string | null => (c === "amd" ? "amd" : c === "xess" ? "intel" : null);

/** The vendor component a choice needs, from vendor_status (null for the game's own, or when the launcher knows none). */
export const componentFor = (c: UpscalerChoice, components: readonly VendorComponent[] | undefined) =>
  components?.find((x) => x.vendor === vendorOf(c)) ?? null;

/** The choice's name ("AMD FSR", "Intel XeSS", "The game's own FSR 3"). */
export const choiceName = (c: UpscalerChoice | string) =>
  t(`settings.graphics.upscaler.choice.${c === "amd" || c === "xess" ? c : "game"}`);

/** A reason code as this launcher knows it ("unknown" for one it does not). */
export const reasonOf = (code: string | null | undefined): UpscalerReason | null =>
  !code ? null : (UPSCALER_REASONS as readonly string[]).includes(code) ? (code as UpscalerReason) : "unknown";

const TOOLS = ["optiscaler", "streamline", "vkfg", "dlsstweaks", "nvngx", "ngxPatched", "overlayOff", "upscaler"];

/** The tools Kiln's upscaler steps aside for, by name ("OptiScaler, another upscaler tool (...)"). */
export const toolNames = (codes: readonly string[]) =>
  codes.map((c) => t(`settings.graphics.upscaler.tools.${TOOLS.includes(c) ? c : "unknown"}`)).join(", ");

/** The shared text of a reason, filled. */
export function reasonText(reason: UpscalerReason, choice: UpscalerChoice | string, tools: readonly string[] = [], number?: number): string {
  return t(`settings.graphics.upscaler.reason.${reason}`, { upscaler: choiceName(choice), tools: toolNames(tools), number: number ?? "?" });
}

export interface UpscalerView {
  /** "now": the running game's file; "last": the last game's; "plan": what Kiln will do (no file, or the choice
   *  changed since the last game). Null: nothing to say (the game's own chosen, nothing in a file). */
  heading: "now" | "last" | "plan" | null;
  lines: string[];
  /** kiln.ui's English detail of the reason, for a muted line (the file's only). */
  detail?: string;
  /** The choice needs files that are not in the game (or not as Kiln pinned them): offer this component's download. */
  offer: VendorComponent | null;
  /** The running game must restart for the choice to run ("Restart the game to use AMD FSR..."): shown first, as a
   *  warning, so it is not missed (a player took the game's FSR 3 for AMD FSR 3.1, 29.19 beta 2). */
  restart?: string;
}

/** The running game needs a restart for `choice`: Kiln's swap goes in only for the choice the game started with. From
 *  the game's file: its reason (restart, for the choice it has), else what it started with as far as the file shows (a
 *  vendor's upscaler running, or Kiln's hooks not in at all). */
export function restartNeeded(choice: UpscalerChoice, state: GraphicsNow | undefined | null): boolean {
  const s = state?.state.upscaler;
  if (!state?.current || !s || choice === "game") return false;
  if (s.chosen === choice) return reasonOf(s.reason) === "restart";
  const started = s.running === "amdFsr4" || s.running === "amdFsr31" ? "amd" : s.running === "xess" ? "xess" : s.hooked === false ? "game" : null;
  return started !== null && started !== choice;
}

/** What the Graphics card and the Deck say about the upscaler. `aside`: GameStatus.graphicsStepAside. */
export function upscalerView(choice: UpscalerChoice, amd: AmdUpscalerMode, state: GraphicsNow | undefined | null,
  components: readonly VendorComponent[] | undefined, aside: readonly GraphicsStepAside[] = []): UpscalerView {
  const c = componentFor(choice, components);
  const filesThere = !!c && (c.state === "downloaded" || c.state === "updateAvailable");
  const offer = c && !filesThere && c.state !== "unavailable" ? c : null;
  const tools = aside.filter((a) => a.upscaler).map((a) => a.kind);
  // the game's own file: while that game runs, or after it when its choice is the one set now and what it said still
  // holds (files downloaded since, or the other tool gone, make it the plan again)
  const s = state?.state.upscaler;
  const last = reasonOf(s?.reason);
  const stale = !!state && !state.current && (((last === "filesMissing" || last === "filesBad") && filesThere) || (last === "aside" && !tools.length));
  if (state && s && !stale && (state.current || (s.chosen === choice && (choice !== "amd" || s.amdUpscaler === amd)))) {
    if (s.chosen === "game" && choice === "game" && (s.running === "off" || s.running === "unknown")) return { heading: null, lines: [], offer: null };
    const lines = [t(`settings.graphics.upscaler.running.${["off", "notFsr", "gameFsr3", "amdFsr4", "amdFsr31", "xess"].includes(s.running) ? s.running : "unknown"}`)];
    if (s.version && ["amdFsr4", "amdFsr31", "xess"].includes(s.running)) lines.push(t("settings.graphics.upscaler.version", { version: s.version }));
    if (s.running === "amdFsr31" && s.chosen === "amd" && s.amdUpscaler === "fsr4" && !s.amdPinned) lines.push(t("settings.graphics.upscaler.amdFsr3Auto"));
    const why = reasonOf(s.reason);
    if (why) lines.push(reasonText(why, s.chosen, s.aside ?? [], s.reasonNumber));
    // the choice changed while that game runs: when it applies
    if (state.current && (s.chosen !== choice || (choice === "amd" && s.amdUpscaler !== amd))) lines.push(t("settings.graphics.upscaler.changedNow", { upscaler: choiceName(choice) }));
    const restart = restartNeeded(choice, state) ? t("settings.graphics.upscaler.restartNow", { upscaler: choiceName(choice) }) : undefined;
    return { heading: state.current ? "now" : "last", lines, detail: why && s.detail ? s.detail : undefined, offer, ...(restart ? { restart } : {}) };
  }
  if (choice === "game") return { heading: null, lines: [], offer: null };
  const lines: string[] = [];
  if (tools.length) lines.push(reasonText("aside", choice, tools));
  // the files' status not known (vendor_status still hashing on the first open, or unreadable): say nothing about them,
  // rather than "not available in this build" for a moment
  else if (!components) return { heading: null, lines: [], offer: null };
  else if (!c || c.state === "unavailable") lines.push(reasonText("unavailable", choice));
  else if (c.state === "damaged") lines.push(reasonText("filesBad", choice));
  else if (!filesThere) lines.push(reasonText("filesMissing", choice));
  else lines.push(t(`settings.graphics.upscaler.plan.${choice === "amd" ? (amd === "fsr3" ? "amdFsr3" : amd === "fsr4" ? "amdFsr4" : "amd") : "xess"}`));
  return { heading: "plan", lines, offer };
}
