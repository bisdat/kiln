// scripts/admin-ui-dist.ts, the check at the end of `bun run build:admin-ui`: a built @kiln/admin-ui that bundles a
// launcher-only module fails, whether or not anything of Tauri comes with it, and so does one with an undeclared
// package or a chunk the check cannot read. Fake builds (a bundle, its source map, the styles, the entry's declarations).
import { afterAll, describe, expect, test } from "bun:test";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, relative, resolve } from "node:path";

const LAUNCHER = resolve(import.meta.dir, "..");
const SCRIPT = join(LAUNCHER, "scripts", "admin-ui-dist.ts");
const made: string[] = [];
afterAll(() => { for (const d of made) rmSync(d, { recursive: true, force: true }); });

const BUNDLE = 'import { jsx } from "react/jsx-runtime";\nimport { create } from "zustand";\nexport const x = [jsx, create];\n';
/** a source map naming `sources` (launcher-relative) the way vite writes them: relative to the dist folder */
const mapOf = (dist: string, sources: string[]) => JSON.stringify({ version: 3, sources: sources.map((s) => relative(dist, join(LAUNCHER, s))), mappings: "" });

function build(sources: string[], files: Record<string, string | ((dist: string) => string)> = {}) {
  const dist = mkdtempSync(join(tmpdir(), "kiln-admin-ui-dist-"));
  made.push(dist);
  mkdirSync(join(dist, "types", "admin"), { recursive: true });
  writeFileSync(join(dist, "admin-ui.js"), BUNDLE);
  writeFileSync(join(dist, "admin-ui.js.map"), mapOf(dist, sources));
  writeFileSync(join(dist, "admin-ui.css"), "");
  writeFileSync(join(dist, "types", "admin", "web.d.ts"), "export declare const x: unknown[];\n");
  for (const [name, text] of Object.entries(files)) writeFileSync(join(dist, name), typeof text === "function" ? text(dist) : text);
  const p = Bun.spawnSync([process.execPath, SCRIPT, dist], { stdout: "pipe", stderr: "pipe" });
  return { code: p.exitCode, out: p.stdout.toString() + p.stderr.toString() };
}

const OWN = ["src/admin/index.tsx", "src/admin/pages/AdminSavesPage.tsx", "src/api/admin.ts", "src/api/adminWs.ts", "src/shell/serverNav.ts", "src/ui/Modal.tsx", "src/i18n/en.json"];

describe("admin-ui-dist", () => {
  test("a bundle of the package's own modules passes", () => {
    const r = build([...OWN, "\0vite/preload-helper.js"]);
    expect(r.out).toContain("@kiln/admin-ui: ok");
    expect(r.code).toBe(0);
  });

  // none of these brings Tauri along: only the source map shows them
  for (const leak of ["src/admin/mock/index.ts", "src/admin/mock/world.ts", "src/api/mock.ts", "src/api/fallback.ts", "src/components/LanguageSettings.tsx", "src/shell/nav.ts", "src/pages/servers/listSay.ts", "src/admin/harness/main.tsx"]) {
    test(`a bundled launcher-only module fails: ${leak}`, () => {
      const r = build([...OWN, leak]);
      expect(r.code).toBe(1);
      expect(r.out).toContain(`admin-ui.js bundles ${leak}`);
    });
  }

  test("a bundled package must be one the package declares", () => {
    expect(build([...OWN, "node_modules/zustand/esm/index.mjs", "node_modules/@tanstack/react-virtual/dist/esm/index.js"]).code).toBe(0);
    const r = build([...OWN, "node_modules/marked/lib/marked.esm.js", "node_modules/@scope/thing/index.js"]);
    expect(r.code).toBe(1);
    expect(r.out).toContain("admin-ui.js bundles marked, which packages/admin-ui/package.json does not declare");
    expect(r.out).toContain("admin-ui.js bundles @scope/thing,");
  });

  test("a chunk split off the bundle is checked like the bundle, and one without a source map fails", () => {
    const leak = build(OWN, { "world-abc123.js": "export const w = 1;\n", "world-abc123.js.map": (d) => mapOf(d, ["src/admin/mock/world.ts"]) });
    expect(leak.code).toBe(1);
    expect(leak.out).toContain("world-abc123.js bundles src/admin/mock/world.ts");
    const blind = build(OWN, { "zones-abc123.js": "export const z = 1;\n" });
    expect(blind.code).toBe(1);
    expect(blind.out).toContain("zones-abc123.js has no source map");
    const tauri = build(OWN, { "x-1.js": "window.__TAURI_INTERNALS__;\n", "x-1.js.map": (d) => mapOf(d, ["src/api/adminWs.ts"]) });
    expect(tauri.code).toBe(1);
    expect(tauri.out).toContain("x-1.js mentions Tauri");
  });

  test("declarations that reach a launcher-only file fail", () => {
    const r = build(OWN, {});
    expect(r.code).toBe(0);
    const dist = mkdtempSync(join(tmpdir(), "kiln-admin-ui-dist-"));
    made.push(dist);
    mkdirSync(join(dist, "types", "admin"), { recursive: true });
    mkdirSync(join(dist, "types", "shell"), { recursive: true });
    writeFileSync(join(dist, "admin-ui.js"), BUNDLE);
    writeFileSync(join(dist, "admin-ui.js.map"), mapOf(dist, OWN));
    writeFileSync(join(dist, "admin-ui.css"), "");
    writeFileSync(join(dist, "types", "admin", "web.d.ts"), 'export type { Nav } from "../shell/nav";\n');
    writeFileSync(join(dist, "types", "shell", "nav.d.ts"), "export type Nav = string;\n");
    const p = Bun.spawnSync([process.execPath, SCRIPT, dist], { stdout: "pipe", stderr: "pipe" });
    expect(p.exitCode).toBe(1);
    expect(p.stdout.toString() + p.stderr.toString()).toContain("the declarations reach shell/nav.d.ts");
  });
});
