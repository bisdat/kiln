import { expect, test } from "bun:test";
import { dialogDefault } from "../src/ui/dialogFolder";

test("a Save dialog opens at the backend's folder with the suggested file name", async () => {
  const asked: unknown[] = [];
  const p = await dialogDefault("supportReport", { fileName: "kiln-report-2026-11-20.zip" }, async (...a) => { asked.push(a); return "/home/p/Downloads/kiln-report-2026-11-20.zip"; });
  expect(p).toBe("/home/p/Downloads/kiln-report-2026-11-20.zip");
  expect(asked).toEqual([["supportReport", "kiln-report-2026-11-20.zip", null]]);
});

test("a caller's path goes to the backend as the preferred one", async () => {
  const asked: unknown[] = [];
  await dialogDefault("folder", { preferred: "/mnt/games" }, async (...a) => { asked.push(a); return "/mnt/games"; });
  expect(asked).toEqual([["folder", null, "/mnt/games"]]);
});

test("no folder from the backend, or no backend: the caller's own path or file name, as before", async () => {
  expect(await dialogDefault("addMods", {}, async () => null)).toBeUndefined();
  expect(await dialogDefault("supportReport", { fileName: "r.zip" }, async () => null)).toBe("r.zip");
  expect(await dialogDefault("folder", { preferred: "/mnt/games" }, async () => { throw new Error("not_implemented"); })).toBe("/mnt/games");
});

test("the browser mock answers with Downloads", async () => {
  expect(await dialogDefault("supportReport", { fileName: "r.zip" })).toBe("/home/player/Downloads/r.zip");
  expect(await dialogDefault("addMods")).toBe("/home/player/Downloads");
});
