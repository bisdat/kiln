// (0.2.0-dev.31.25, Soligna, Nexus 301) "Draw Kiln's overlay anyway": the player's switch for Kiln's in-game overlay next
// to a tool it steps aside for on its own (NVIDIA Streamline; OptiScaler with its own menu or frame generation). On writes
// kiln.json "ui": {"overlay": "on"}, which kiln.ui reads at the game's start as KILN_UI_OVERLAY=1 (Steam's launch options
// cannot set that variable on Windows): the overlay draws next to those tools, and at every start, also after one that
// closed while the overlay was starting (the crash guard still records each attempt; native/services/ui/overlay_setting.h,
// docs/kiln/ui.md). Off removes the key: Kiln decides again. A trade of stability for function, so it is only ever the
// player's choice, made here, never Kiln's. Shown on the Play page (under the tool's banner) and in Settings, Graphics
// (under the step-aside lines), and in Graphics also while the setting is on with no such tool, so it can be switched off.
import type { GameId, GraphicsStepAside, GraphicsToolNote, Json } from "../api/commands";
import { useT } from "../i18n";
import { useKilnJson } from "../pages/settings/kilnJson";
import { Toggle } from "../ui";

/** The kiln.json entry the switch writes. */
export const OVERLAY_PATH = "ui.overlay";

/** kiln.json's values: the overlay drawn anyway ("on", exactly: kiln.ui takes no other spelling). */
export function overlayAnyway(values: Json | null | undefined): boolean {
  const ui = values && typeof values === "object" && !Array.isArray(values) ? (values as Record<string, Json>).ui : undefined;
  return !!ui && typeof ui === "object" && !Array.isArray(ui) && (ui as Record<string, Json>).overlay === "on";
}

/** The tools in the game folder next to which Kiln's overlay steps aside on its own, by name: NVIDIA Streamline
 *  (GameStatus.graphicsStepAside, `sl.interposer.dll` in the folder or `Streamline/`, as kiln.ui looks for it), and
 *  OptiScaler where its own ini makes Kiln step aside (GameStatus.graphicsTools, `kilnOverlay` false). */
export function overlayAsideTools(aside: GraphicsStepAside[], tools: GraphicsToolNote[]): string[] {
  const out: string[] = [];
  if (aside.some((a) => a.kind === "streamline")) out.push("NVIDIA Streamline");
  if (tools.some((g) => g.engine === "optiscaler" && !g.kilnOverlay)) out.push("OptiScaler");
  return out;
}

/** The switch itself: what it does, next to which tools, and what it risks. */
export function OverlayAnywaySwitch({ on, tools, disabled, onChange }: { on: boolean; tools: string[]; disabled?: boolean; onChange: (on: boolean) => void }) {
  const t = useT();
  return (
    <div data-testid="overlay-anyway">
      <Toggle checked={on} disabled={disabled} onChange={onChange} label={t("settings.graphics.overlayAnyway.label")}
        hint={tools.length ? t("settings.graphics.overlayAnyway.hint", { tools: tools.join(", ") }) : t("settings.graphics.overlayAnyway.hintNoTool")} />
    </div>
  );
}

/** The switch for a game: reads and writes its kiln/kiln.json (Settings' query, so every place shows the same). */
export function OverlayAnyway({ gameId, tools }: { gameId: GameId; tools: string[] }) {
  const k = useKilnJson(gameId);
  const doc = k.doc.data;
  return (
    <OverlayAnywaySwitch on={overlayAnyway(doc?.values)} tools={tools} disabled={!doc || k.busy}
      onChange={(on) => (on ? k.set.mutate({ path: OVERLAY_PATH, value: "on" }) : k.reset.mutate(OVERLAY_PATH))} />
  );
}
