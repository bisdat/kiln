// (dev.29.18) The upscaler behind the game's FSR 3 option as the launcher says it (src/components/upscalerStatus.ts):
// the game's kiln/state/graphics.json while it runs ("Now") and after ("Last game"), else the plan from the choice, the
// files and the tools in the game folder; and the reason texts, one table with the game's (runtime/locales/en.json)
// and the docs (docs/kiln/graphics.md).
import { expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { UPSCALER_REASONS, type GraphicsNow, type GraphicsStateFile } from "../src/api/commands";
import type { VendorComponent } from "../src/api/vendor";
import { amdModeOf, choiceOf, componentFor, reasonOf, upscalerView } from "../src/components/upscalerStatus";
import en from "../src/i18n/en.json";

const repo = join(import.meta.dir, "..", "..");
const runtime = JSON.parse(readFileSync(join(repo, "runtime", "locales", "en.json"), "utf8"));
const snake = (k: string) => k.replace(/[A-Z]/g, (c) => "_" + c.toLowerCase());

const comp = (vendor: "amd" | "intel", patch: Partial<VendorComponent> = {}): VendorComponent => ({
  id: vendor === "amd" ? "amd-fsr" : "intel-xess", vendor, vendorName: vendor === "amd" ? "AMD" : "Intel",
  name: vendor === "amd" ? "AMD FSR" : "Intel XeSS", enables: "x", version: vendor === "amd" ? "2.4.0" : "3.0.2", source: null,
  license: { name: "MIT licence", sha256: "ab".repeat(32), from: "https://github.com/x", agreed: false }, hosts: ["raw.githubusercontent.com"],
  size: 72_981_808, files: [{ name: "a.dll", size: 1, pinned: true }], state: "notDownloaded", installed: null, partial: 0, reason: null, ...patch,
});
const both = (amdState: VendorComponent["state"] = "notDownloaded", intelState: VendorComponent["state"] = "notDownloaded") =>
  [comp("amd", { state: amdState }), comp("intel", { state: intelState })];
const file = (upscaler: Partial<GraphicsStateFile["upscaler"]>): GraphicsStateFile => ({
  format: 1, pid: 42, written: "2026-11-18T22:00:00Z", kiln: "0.2.0-dev.29.18",
  upscaler: { chosen: "amd", amdUpscaler: "auto", running: "amdFsr4", reason: null, ...upscaler },
  files: { amd: { version: "2.4.0", state: "present" } },
});
const now = (u: Partial<GraphicsStateFile["upscaler"]>, current = true): GraphicsNow => ({ current, state: file(u) });

test("the choice is read from kiln.json, the game's own for anything else", () => {
  expect(choiceOf(undefined)).toBe("game");
  expect(choiceOf({ graphics: { upscaler: "amd" } })).toBe("amd");
  expect(choiceOf({ graphics: { upscaler: "xess" } })).toBe("xess");
  expect(choiceOf({ graphics: { upscaler: "dlss" } })).toBe("game");
  expect(amdModeOf({ graphics: { amdUpscaler: "fsr3" } })).toBe("fsr3");
  expect(amdModeOf({ graphics: { amdUpscaler: 1 } })).toBe("auto");
  expect(componentFor("xess", both())?.id).toBe("intel-xess");
  expect(componentFor("game", both())).toBeNull();
  expect(reasonOf("filesMissing")).toBe("filesMissing");
  expect(reasonOf("somethingNew")).toBe("unknown");
  expect(reasonOf(null)).toBeNull();
});

test("the game's own chosen and nothing in a file: nothing to say, nothing offered", () => {
  expect(upscalerView("game", "auto", undefined, both())).toEqual({ heading: null, lines: [], offer: null });
  expect(upscalerView("game", "auto", now({ chosen: "game", running: "off" }), both()).heading).toBeNull();
});

test("the plan: what Kiln will run, or why not, and the offer of the files", () => {
  const missing = upscalerView("amd", "auto", undefined, both());
  expect(missing.heading).toBe("plan");
  expect(missing.lines).toEqual(["AMD FSR's files are not downloaded yet, so the game's own FSR 3 runs."]);
  expect(missing.offer?.id).toBe("amd-fsr");
  const ready = upscalerView("amd", "auto", undefined, both("downloaded"));
  expect(ready.lines).toEqual(["With FSR 3 chosen in the game's graphics settings, Kiln runs AMD FSR in its place: FSR 4 where your graphics card and driver run it, else FSR 3.1."]);
  expect(ready.offer).toBeNull();
  expect(upscalerView("amd", "fsr3", undefined, both("downloaded")).lines[0]).toBe("With FSR 3 chosen in the game's graphics settings, Kiln runs AMD FSR 3.1 in its place.");
  expect(upscalerView("xess", "auto", undefined, both("downloaded", "updateAvailable")).lines[0]).toContain("Kiln runs Intel XeSS in its place");
  // changed files: download again
  const damaged = upscalerView("xess", "auto", undefined, both("downloaded", "damaged"));
  expect(damaged.lines[0]).toBe("A file of Intel XeSS is not the one Kiln expects (its size or SHA-256 differs), so Kiln does not load it. Download it again.");
  expect(damaged.offer?.id).toBe("intel-xess");
  // not in this build: nothing to offer
  const none = upscalerView("amd", "auto", undefined, both("unavailable"));
  expect(none.lines[0]).toBe("AMD FSR is not available in this build of Kiln or on this system, so the game's own FSR 3 runs.");
  expect(none.offer).toBeNull();
  // the files' status not read yet (vendor_status hashes the files on the first open) or unreadable: nothing said
  // about them, no "not available in this build" for a moment
  expect(upscalerView("amd", "auto", undefined, undefined)).toEqual({ heading: null, lines: [], offer: null });
  expect(upscalerView("xess", "auto", undefined, undefined, [{ kind: "optiscaler", file: "dxgi.dll", frameCap: true, upscaler: true }]).lines)
    .toEqual(["Kiln steps aside for OptiScaler, so it does not change the game's FSR 3."]);
  // a tool Kiln steps aside for wins over the files; DLSSTweaks does not count
  const aside = upscalerView("amd", "auto", undefined, both("downloaded"), [
    { kind: "dlsstweaks", file: "dlsstweaks.ini", frameCap: false, upscaler: false },
    { kind: "optiscaler", file: "dxgi.dll", frameCap: true, upscaler: true },
  ]);
  expect(aside.lines).toEqual(["Kiln steps aside for OptiScaler, so it does not change the game's FSR 3."]);
  expect(upscalerView("amd", "auto", undefined, both("downloaded"), [{ kind: "dlsstweaks", file: "d.ini", frameCap: false }]).lines[0]).toContain("Kiln runs AMD FSR");
});

test("now: what the running game wrote, with the version and the reason", () => {
  const fsr4 = upscalerView("amd", "auto", now({ running: "amdFsr4", version: "4.1.2" }), both("downloaded"));
  expect(fsr4).toEqual({ heading: "now", lines: ["AMD FSR 4 runs in place of the game's FSR 3.", "Version 4.1.2."], detail: undefined, offer: null });
  const auto31 = upscalerView("amd", "auto", now({ running: "amdFsr31", version: "3.1.6" }), both("downloaded"));
  expect(auto31.lines).toEqual(["AMD FSR 3.1 runs in place of the game's FSR 3.", "Version 3.1.6.", "AMD's selection chose FSR 3.1: FSR 4 does not run on this graphics card or driver."]);
  expect(upscalerView("amd", "fsr3", now({ amdUpscaler: "fsr3", running: "amdFsr31", amdPinned: true }), both("downloaded")).lines).toHaveLength(1);
  const device = upscalerView("amd", "auto", now({ running: "gameFsr3", reason: "device", detail: "VK_KHR_shader_float16_int8 is missing" }), both("downloaded"));
  expect(device.lines).toEqual(["The game's own FSR 3 runs.", "Your graphics card or driver lacks a Vulkan feature AMD FSR needs, so the game's own FSR 3 runs."]);
  expect(device.detail).toBe("VK_KHR_shader_float16_int8 is missing");
  const aside = upscalerView("xess", "auto", now({ chosen: "xess", running: "off", reason: "aside", aside: ["optiscaler", "upscaler"] }), both());
  expect(aside.lines[1]).toBe("Kiln steps aside for OptiScaler, another upscaler tool (AMD's or Intel's upscaler files next to the game, or a changed FSR 3 pass), so it does not change the game's FSR 3.");
  expect(upscalerView("amd", "auto", now({ running: "notFsr" }), both("downloaded")).lines[0]).toContain("not upscaling with FSR 3 right now");
  // a newer runtime's reason and running state
  const newer = upscalerView("amd", "auto", now({ running: "amdFsr5", reason: "brandNew", reasonNumber: 40 }), both("downloaded"));
  expect(newer.lines).toEqual(["Kiln cannot tell what upscales behind the game's FSR 3: its parts are of different versions.", "AMD FSR does not run (reason 40 from a newer Kiln), so the game's own FSR 3 runs."]);
  // the choice changed in the launcher while that game runs: when it applies
  const changed = upscalerView("xess", "auto", now({ running: "amdFsr4" }), both("downloaded", "downloaded"));
  expect(changed.heading).toBe("now");
  expect(changed.lines.at(-1)).toBe("You have chosen Intel XeSS since that game started: it applies when the game next sets FSR up, or from its next start.");
});

test("the last game: shown while its choice is the one set and what it said still holds", () => {
  const last = upscalerView("amd", "auto", now({ running: "amdFsr4", version: "4.1.2" }, false), both("downloaded"));
  expect(last.heading).toBe("last");
  // another choice since: the plan for it
  expect(upscalerView("xess", "auto", now({ running: "amdFsr4" }, false), both("downloaded", "downloaded")).heading).toBe("plan");
  expect(upscalerView("amd", "fsr3", now({ running: "amdFsr4" }, false), both("downloaded")).heading).toBe("plan");
  // files missing then, downloaded since: the plan
  const since = upscalerView("amd", "auto", now({ running: "gameFsr3", reason: "filesMissing" }, false), both("downloaded"));
  expect(since.heading).toBe("plan");
  expect(since.lines[0]).toContain("Kiln runs AMD FSR in its place");
  // still missing: the last game's words, and the offer
  const still = upscalerView("amd", "auto", now({ running: "gameFsr3", reason: "filesMissing" }, false), both());
  expect(still.heading).toBe("last");
  expect(still.offer?.id).toBe("amd-fsr");
  // the other tool gone since: the plan
  expect(upscalerView("amd", "auto", now({ running: "off", reason: "aside", aside: ["optiscaler"] }, false), both("downloaded")).heading).toBe("plan");
});

test("one table of reasons: kiln_ui.h's, the game's English, the launcher's English and the docs", () => {
  const h = readFileSync(join(repo, "native", "core", "abi", "services", "kiln_ui.h"), "utf8");
  const why = [...h.matchAll(/\bKILN_UI_UPSCALE_WHY_([A-Z0-9_]+)\s*=\s*(\d+)/g)].map((m) => [m[1], Number(m[2])] as const);
  const camel = (s: string) => s.toLowerCase().replace(/_([a-z0-9])/g, (_, c: string) => c.toUpperCase());
  expect(why.filter(([, n]) => n > 0).map(([name]) => camel(name))).toEqual([...UPSCALER_REASONS]);
  const game = runtime.graphics.upscaler.reason as Record<string, string>;
  const mine = (en as unknown as { settings: { graphics: { upscaler: { reason: Record<string, string>; running: Record<string, string>; tools: Record<string, string> } } } })
    .settings.graphics.upscaler;
  const codes = [...UPSCALER_REASONS, "unknown"];
  expect(Object.keys(mine.reason).sort()).toEqual([...codes].sort());
  expect(Object.keys(game).sort()).toEqual(codes.map(snake).sort());
  const docs = readFileSync(join(repo, "docs", "kiln", "graphics.md"), "utf8");
  for (const c of codes) {
    expect(mine.reason[c]).toBe(game[snake(c)]);
    expect(mine.reason[c]).not.toMatch(/dlss|unlock/i);
    expect(docs).toContain(`| \`${c}\` |`);
  }
  // what runs, in the same words as the game's page
  for (const [k, v] of Object.entries(mine.running)) expect(v).toBe(runtime.graphics.upscaler.running[snake(k)]);
  // the tools by name, as the game's page names them
  for (const k of ["optiscaler", "streamline", "vkfg", "dlsstweaks", "nvngx", "upscaler"]) expect(mine.tools[k]).toBe(runtime.graphics.aside[k]);
  expect(mine.tools.ngxPatched).toBe(runtime.graphics.aside.ngx_patched);
  expect(mine.tools.overlayOff).toBe(runtime.graphics.aside.overlay_off);
});

test("the schema's example is what the launcher reads", () => {
  const example = JSON.parse(readFileSync(join(repo, "schema", "examples", "state-graphics.json"), "utf8")) as GraphicsStateFile;
  const v = upscalerView("amd", "auto", { current: true, state: example }, both("downloaded"));
  expect(v.heading).toBe("now");
  expect(v.lines[0]).toBe("AMD FSR 4 runs in place of the game's FSR 3.");
});
