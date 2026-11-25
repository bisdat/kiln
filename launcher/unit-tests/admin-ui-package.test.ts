// @kiln/admin-ui (launcher/packages/admin-ui, a workspace package built from launcher/src/admin): the launcher's admin
// pages for hosts outside the Tauri app (Kilnwarden). Its entry (src/admin/web.ts) and the WebSocket transport must
// not reach Tauri, the launcher's command layer or its browser mock, neither at run time nor through the types its
// declarations carry.
import { describe, expect, test } from "bun:test";
import { existsSync, readFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";

const LAUNCHER = resolve(import.meta.dir, "..");
const SRC = join(LAUNCHER, "src");
const transpiler = new Bun.Transpiler({ loader: "tsx" });
const EXTS = ["", ".ts", ".tsx", "/index.ts", "/index.tsx"];

function resolveRel(from: string, spec: string): string | null {
  for (const e of EXTS) {
    const p = resolve(dirname(from), spec + e);
    if (existsSync(p) && !p.endsWith("/") && (e !== "" || /\.(tsx?|json|css|svg)$/.test(p))) return p;
  }
  return null;
}

/** Every file and bare package `entry` reaches: at run time (`types: false`) or also through type-only imports. */
function graph(entry: string, types: boolean) {
  const files = new Set<string>();
  const bare = new Set<string>();
  const todo = [resolve(entry)];
  while (todo.length) {
    const f = todo.pop()!;
    if (files.has(f)) continue;
    files.add(f);
    if (!/\.tsx?$/.test(f)) continue;
    const code = readFileSync(f, "utf8");
    const specs = transpiler.scanImports(code).map((i) => i.path);
    if (types) for (const m of code.matchAll(/^\s*(?:import|export)\s+type\s[^;]*?from\s+["']([^"']+)["']/gm)) specs.push(m[1]);
    if (types) for (const m of code.matchAll(/import\(["']([^"']+)["']\)\.\w+/g)) specs.push(m[1]);
    for (const s of specs) {
      if (s.startsWith(".")) {
        const p = resolveRel(f, s.replace(/\?.*$/, ""));
        if (!p) throw new Error(`${relative(LAUNCHER, f)}: cannot resolve ${s}`);
        todo.push(p);
      } else bare.add(s);
    }
  }
  return { files: [...files].map((f) => relative(LAUNCHER, f)), bare: [...bare] };
}

const LAUNCHER_ONLY = [/^src\/api\/commands\.ts$/, /^src\/api\/mock/, /^src\/api\/adminTauri\.ts$/, /^src\/api\/fallback\.ts$/, /^src\/components\//, /^src\/ui\/(TaskProgress|DropZone|dialogFolder)\./, /^src\/pages\//, /^src\/shell\/(?!serverNav\.ts$)/, /^src\/admin\/mock\//];

describe("@kiln/admin-ui", () => {
  test("is a workspace package with an entry, its types and its styles", () => {
    const pkg = JSON.parse(readFileSync(join(LAUNCHER, "packages", "admin-ui", "package.json"), "utf8"));
    expect(pkg.name).toBe("@kiln/admin-ui");
    expect(pkg.private).toBe(true);
    expect(pkg.exports["."]).toEqual({ types: "./dist/types/admin/web.d.ts", import: "./dist/admin-ui.js" });
    expect(pkg.exports["./style.css"]).toBe("./dist/admin-ui.css");
    for (const p of ["react", "react-dom", "react-router-dom", "@tanstack/react-query"]) expect(pkg.peerDependencies[p]).toBeString();
    const launcher = JSON.parse(readFileSync(join(LAUNCHER, "package.json"), "utf8"));
    expect(launcher.workspaces).toContain("packages/admin-ui");
    expect(launcher.scripts["build:admin-ui"]).toBeString();
  });

  test("src/admin itself has no package.json: the launcher's import(\"./admin\") stays src/admin/index.tsx", () => {
    // a package.json there would make bundlers resolve the folder through its exports (the built bundle, with a hub of
    // its own that the launcher never configured)
    expect(existsSync(join(SRC, "admin", "package.json"))).toBe(false);
    expect(readFileSync(join(SRC, "App.tsx"), "utf8")).toContain('import("./admin")');
  });

  for (const types of [false, true]) {
    test(`the entry reaches no Tauri, command layer or mock ${types ? "through its types" : "at run time"}`, () => {
      const g = graph(join(SRC, "admin", "web.ts"), types);
      expect(g.bare.filter((b) => b.startsWith("@tauri-apps/"))).toEqual([]);
      expect(g.files.filter((f) => LAUNCHER_ONLY.some((r) => r.test(f)))).toEqual([]);
      // what it is made of: the admin pages, the hub, the WebSocket transport, the design system, the texts
      for (const f of ["src/admin/index.tsx", "src/api/admin.ts", "src/api/adminWs.ts", "src/ui/Modal.tsx", "src/i18n/index.ts"]) expect(g.files).toContain(f);
    });
  }

  test("the WebSocket transport alone reaches nothing of the launcher's backend", () => {
    const g = graph(join(SRC, "api", "adminWs.ts"), true);
    expect(g.bare.filter((b) => b.startsWith("@tauri-apps/"))).toEqual([]);
    expect(g.files.filter((f) => /^src\/api\/(commands|adminTauri|mock)/.test(f))).toEqual([]);
  });

  test("the launcher installs the Tauri transport before anything renders", () => {
    const main = readFileSync(join(SRC, "main.tsx"), "utf8");
    expect(main).toMatch(/^import "\.\/api\/adminTauri";/m);
    // the launcher's own transport is where Tauri is
    const own = transpiler.scanImports(readFileSync(join(SRC, "api", "adminTauri.ts"), "utf8")).map((i) => i.path);
    expect(own).toContain("@tauri-apps/api/core");
    expect(own).toContain("./commands");
  });
});
