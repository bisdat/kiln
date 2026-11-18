import { expect, test } from "bun:test";
import type { SchemaSnapshot } from "../src/api/commands";
import { graphicsAsideText, graphicsSchema, isGraphicsPath } from "../src/components/GraphicsSettings";
import snapshot from "../src/data/kiln-json.snapshot.json";

const schema = snapshot as unknown as SchemaSnapshot;

test("the Graphics card edits the graphics.* entries of kiln.json, the kiln.json card the rest", () => {
  const g = graphicsSchema(schema);
  expect(g.sections).toEqual([]);
  expect(g.entries.map((e) => e.path)).toEqual([
    "graphics.fpsCap", "graphics.dlssPreset", "graphics.dlssScale", "graphics.dlssRenderScale", "graphics.dlssQualityScale",
    "graphics.dlssBalancedScale", "graphics.dlssPerformanceScale", "graphics.dlssUltraPerformanceScale", "graphics.readout",
  ]);
  expect(schema.entries.filter((e) => !isGraphicsPath(e.path)).some((e) => e.path.startsWith("graphics"))).toBe(false);
  expect(isGraphicsPath("graphics")).toBe(false);
  expect(isGraphicsPath("graphicsX.a")).toBe(false);
});

test("the snapshot's graphics entries are the ones the game reads", () => {
  const by = Object.fromEntries(schema.entries.map((e) => [e.path, e]));
  expect(schema.sections?.some((s) => s.path === "graphics")).toBe(true);
  expect(by["graphics.fpsCap"]).toMatchObject({ type: "int", default: 0, min: 0, max: 500 });
  expect(by["graphics.dlssPreset"]).toMatchObject({ type: "enum", default: "default", options: ["default", "J", "K", "L", "M"] });
  expect(by["graphics.dlssScale"]).toMatchObject({ type: "enum", default: "game", options: ["game", "perMode", "custom"] });
  for (const [k, d] of [["dlssRenderScale", 0.67], ["dlssQualityScale", 0.667], ["dlssBalancedScale", 0.58], ["dlssPerformanceScale", 0.5],
    ["dlssUltraPerformanceScale", 0.333]] as const)
    expect(by[`graphics.${k}`]).toMatchObject({ type: "float", default: d, min: 0.33, max: 1 });
  expect(by["graphics.readout"]).toMatchObject({ type: "bool", default: false });
  for (const e of schema.entries.filter((e) => isGraphicsPath(e.path))) {
    expect(e.requiresRestart).toBe(false);
    expect(e.comment?.length ?? 0).toBeGreaterThan(10);
  }
});

test("a step-aside line names the tool's file and what steps aside", () => {
  expect(graphicsAsideText({ kind: "optiscaler", file: "dxgi.dll", frameCap: true })).toBe(
    "OptiScaler is installed (dxgi.dll): Kiln's frame cap and DLSS settings step aside; the readout still works.");
  expect(graphicsAsideText({ kind: "dlsstweaks", file: "dlsstweaks.ini", frameCap: false })).toBe(
    "DLSSTweaks is installed (dlsstweaks.ini): Kiln's DLSS settings step aside; the frame cap and the readout still work.");
  expect(graphicsAsideText({ kind: "streamline", file: "Streamline/sl.interposer.dll", frameCap: true })).toContain("Streamline/sl.interposer.dll");
  expect(graphicsAsideText({ kind: "vkfg", file: "EnshroudedVKFG.ini", frameCap: true })).toContain("frame generation");
  expect(graphicsAsideText({ kind: "nvngx", file: "_nvngx.dll", frameCap: false })).toContain("Another DLSS loader");
  // a kind a later backend adds still reads as a sentence
  expect(graphicsAsideText({ kind: "future", file: "x.dll", frameCap: false })).toStartWith("Another graphics tool is in the game folder (x.dll)");
});
