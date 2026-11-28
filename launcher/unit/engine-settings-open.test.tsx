// The Settings window's "Open <file>" button (sosmic, 0.2.0-dev.31.25): it shows the file the engine reads once it is
// there (`openFile`), else a plain line; ShroudForge's Library copy is never offered.
import { expect, test } from "bun:test";
import { renderToStaticMarkup } from "react-dom/server";
import { SettingsFileOpen } from "../src/components/EngineSettings";
import type { EngineSettings } from "../src/api/commands";

const st = (more: Partial<EngineSettings> = {}): EngineSettings => ({
  kind: "shroudtopia", id: "Fly Mode", format: "shroudtopia.json", file: "/data/profiles/p1/config/shroudtopia/shroudtopia.json",
  schema: { type: "object", properties: {} }, values: {}, note: null, ...more,
});
const sf = (more: Partial<EngineSettings> = {}): EngineSettings => ({
  kind: "shroudforge", id: "better-chests", format: "extended.mod.json", file: "/data/library/shroudforge/better-chests/1.0.0+abcd/extended.mod.json",
  schema: { type: "object", properties: {} }, values: {}, note: null, ...more,
});
const render = (s: EngineSettings) => renderToStaticMarkup(<SettingsFileOpen s={s} />);

test("shroudtopia.json: an Open button once it exists, else a plain line", () => {
  const open = render(st({ openFile: "/data/profiles/p1/config/shroudtopia/shroudtopia.json" }));
  expect(open).toContain("<button");
  expect(open).toContain("Open shroudtopia.json");
  const missing = render(st());
  expect(missing).not.toContain("<button");
  expect(missing).toContain("shroudtopia.json doesn&#x27;t exist yet");
});

test("ShroudForge: the game's copy opens; the Library copy is never offered", () => {
  expect(render(sf({ file: "/game/mods/better-chests/extended.mod.json", openFile: "/game/mods/better-chests/extended.mod.json" })))
    .toContain("Open extended.mod.json");
  const library = render(sf());
  expect(library).not.toContain("<button");
  expect(library).toContain("isn&#x27;t in the game&#x27;s mods folder yet");
});

test("kinds without a settings file show nothing", () => {
  expect(render({ ...st(), kind: "emf", format: "none", file: null })).toBe("");
});
