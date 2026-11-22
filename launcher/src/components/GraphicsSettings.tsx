// Settings, Graphics (dev.29.18): Kiln's own graphics tools in the game, the "graphics" section of the game's
// kiln/kiln.json (frame cap, DLSS preset and render scale, readout); the game reads the file again while it runs. The
// section itself (its groups: the tools, "Behind the game's FSR 3", the upscaler files, and the tools Kiln steps aside
// for at its top) is pages/settings/sections/Graphics.tsx; these are its pieces: which kiln.json entries it edits, their
// labels, the step-aside lines (GameStatus.graphicsStepAside, kiln_manage::game::graphics_step_aside).
import type { GraphicsStepAside, SchemaSnapshot } from "../api/commands";
import { t as tr } from "../i18n";

/** The kiln.json section the Graphics card edits. */
export const GRAPHICS_SECTION = "graphics";

/** A kiln.json entry of the Graphics card (`graphics.*`). */
export const isGraphicsPath = (path: string) => path.startsWith(GRAPHICS_SECTION + ".");

/** (phase B) The kiln.json entries of the upscaler choice: the card's own block, not the form. */
export const UPSCALER_PATHS = ["graphics.upscaler", "graphics.amdUpscaler"];

/** The Graphics card's form: the `graphics.*` entries alone but the upscaler's, no section head (the card's title says it). */
export function graphicsSchema(schema: SchemaSnapshot): SchemaSnapshot {
  return { ...schema, sections: [], entries: schema.entries.filter((e) => isGraphicsPath(e.path) && !UPSCALER_PATHS.includes(e.path)) };
}

/** The card's field labels by kiln.json path (SchemaForm's `labels`): the key names ("fpsCap", "dlssScale") do not read as
 *  words. Every `graphics.*` entry of the snapshot has one (unit/graphics-settings.test.tsx). */
export function graphicsLabels(): Record<string, string> {
  return {
    "graphics.fpsCap": tr("settings.graphics.labels.fpsCap"),
    "graphics.dlssPreset": tr("settings.graphics.labels.dlssPreset"),
    "graphics.dlssScale": tr("settings.graphics.labels.dlssScale"),
    "graphics.dlssRenderScale": tr("settings.graphics.labels.dlssRenderScale"),
    "graphics.dlssQualityScale": tr("settings.graphics.labels.dlssQualityScale"),
    "graphics.dlssBalancedScale": tr("settings.graphics.labels.dlssBalancedScale"),
    "graphics.dlssPerformanceScale": tr("settings.graphics.labels.dlssPerformanceScale"),
    "graphics.dlssUltraPerformanceScale": tr("settings.graphics.labels.dlssUltraPerformanceScale"),
    "graphics.readout": tr("settings.graphics.labels.readout"),
  };
}

/** One line for a tool Kiln's graphics tools step aside for: what it is (naming the file) and what steps aside. */
export function graphicsAsideText(a: GraphicsStepAside): string {
  const tools: Record<string, string> = {
    optiscaler: tr("settings.graphics.aside.optiscaler", { file: a.file }),
    streamline: tr("settings.graphics.aside.streamline", { file: a.file }),
    vkfg: tr("settings.graphics.aside.vkfg", { file: a.file }),
    dlsstweaks: tr("settings.graphics.aside.dlsstweaks", { file: a.file }),
    nvngx: tr("settings.graphics.aside.nvngx", { file: a.file }),
    upscaler: tr("settings.graphics.aside.upscaler", { file: a.file }),
  };
  const tool = tools[a.kind] ?? tr("settings.graphics.aside.unknown", { file: a.file });
  const effect = tr(`settings.graphics.aside.${a.frameCap ? (a.upscaler ? "frameCapUpscaler" : "frameCap") : a.upscaler ? "upscalerOnly" : "dlss"}`);
  return tr("settings.graphics.aside.line", { tool, effect });
}
