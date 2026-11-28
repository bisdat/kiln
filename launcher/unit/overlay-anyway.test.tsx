// (0.2.0-dev.31.25, Soligna, Nexus 301) "Draw Kiln's overlay anyway": kiln.json ui.overlay "on" (kiln.ui draws as with
// KILN_UI_OVERLAY=1, native/services/ui/overlay_setting.h), offered next to the tools Kiln's overlay steps aside for on
// its own (NVIDIA Streamline; OptiScaler with its own menu or frame generation). The player's choice, made visibly.
import { expect, test } from "bun:test";
import { renderToStaticMarkup } from "react-dom/server";
import type { SchemaSnapshot } from "../src/api/commands";
import { OVERLAY_PATH, OverlayAnywaySwitch, overlayAnyway, overlayAsideTools } from "../src/components/OverlayAnyway";
import snapshot from "../src/data/kiln-json.snapshot.json";

const schema = snapshot as unknown as SchemaSnapshot;

test("the switch is on only for kiln.json ui.overlay \"on\", exactly as kiln.ui reads it", () => {
  expect(OVERLAY_PATH).toBe("ui.overlay");
  expect(overlayAnyway({ ui: { overlay: "on" } })).toBe(true);
  expect(overlayAnyway({ ui: { look: "game", overlay: "on" } })).toBe(true);
  for (const v of [{ ui: { overlay: "off" } }, { ui: { overlay: "auto" } }, { ui: { overlay: "ON" } }, { ui: { overlay: true } }, { ui: {} }, { overlay: "on" },
    { ui: "on" }, {}, null, undefined])
    expect(overlayAnyway(v as never)).toBe(false);
});

test("the snapshot offers \"on\" next to auto and off, and says what it means", () => {
  const e = schema.entries.find((x) => x.path === OVERLAY_PATH)!;
  expect(e).toMatchObject({ type: "enum", default: "auto", options: ["auto", "off", "on"], requiresRestart: true });
  expect(e.comment).toContain("KILN_UI_OVERLAY=1");
  expect(e.comment).toContain("may crash at start with frame generation");
});

test("the tools next to which the switch is offered: Streamline, and OptiScaler only where Kiln's overlay steps aside", () => {
  const sl = { kind: "streamline", file: "Streamline/sl.interposer.dll", frameCap: true, upscaler: true };
  const opti = { kind: "optiscaler", file: "dxgi.dll", frameCap: true, upscaler: true };
  const tweaks = { kind: "dlsstweaks", file: "dlsstweaks.ini", frameCap: false };
  const stepsAside = { engine: "optiscaler" as const, file: "dxgi.dll", kilnOverlay: false, text: "" };
  const draws = { ...stepsAside, kilnOverlay: true };
  expect(overlayAsideTools([], [])).toEqual([]);
  expect(overlayAsideTools([tweaks], [])).toEqual([]);
  expect(overlayAsideTools([sl, tweaks], [])).toEqual(["NVIDIA Streamline"]);
  // OptiScaler's own ini decides (GameStatus.graphicsTools): its menu off and no frame generation, Kiln draws anyway
  expect(overlayAsideTools([opti], [draws])).toEqual([]);
  expect(overlayAsideTools([opti], [stepsAside])).toEqual(["OptiScaler"]);
  expect(overlayAsideTools([sl, opti], [stepsAside])).toEqual(["NVIDIA Streamline", "OptiScaler"]);
});

test("the switch says what it does and what it risks", () => {
  const noop = () => {};
  const off = renderToStaticMarkup(<OverlayAnywaySwitch on={false} tools={["NVIDIA Streamline"]} onChange={noop} />);
  expect(off).toContain('role="switch"');
  expect(off).not.toContain("checked");
  expect(off).toContain("Draw Kiln&#x27;s overlay anyway (may crash at start with frame generation)");
  expect(off).toContain("next to NVIDIA Streamline");
  expect(off).toContain("If the game then closes at start, switch this off");
  const on = renderToStaticMarkup(<OverlayAnywaySwitch on tools={["NVIDIA Streamline", "OptiScaler"]} onChange={noop} disabled />);
  expect(on).toContain('checked=""');
  expect(on).toContain('disabled=""');
  expect(on).toContain("next to NVIDIA Streamline, OptiScaler");
  // left on with no such tool any more: it still says what it does (Kiln no longer decides at any start)
  const stale = renderToStaticMarkup(<OverlayAnywaySwitch on tools={[]} onChange={noop} />);
  expect(stale).toContain("Draw Kiln&#x27;s overlay anyway");
  expect(stale).toContain("Switch this off to let Kiln decide again");
});
