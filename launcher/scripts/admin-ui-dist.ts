// Finishes and checks the admin pages' package build (@kiln/admin-ui): `bun run build:admin-ui` runs vite
// (vite.admin-ui.config.ts) and tsc (tsconfig.admin-ui.json), then this. It keeps the declarations the entry reaches
// (dist/types/admin/web.d.ts and what it imports), drops the stylesheet imports tsc copies into them (the styles are
// dist/admin-ui.css), and fails when the package reaches what a web page must never load: Tauri, the launcher's
// command layer or its mock, or a package packages/admin-ui/package.json does not declare. What the bundle holds is
// read from its source maps (every module vite put in it), so a launcher-only module fails even when nothing of Tauri
// comes with it (the admin pages' browser mock, a launcher component); the unit test
// (unit-tests/admin-ui-package.test.ts) checks the same on the sources.
//
//   bun scripts/admin-ui-dist.ts [dist folder]          (default packages/admin-ui/dist)
import { existsSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";

const here = resolve(import.meta.dir, "..");
const dist = resolve(process.argv[2] ?? join(here, "packages", "admin-ui", "dist"));
const pkg = JSON.parse(readFileSync(join(here, "packages", "admin-ui", "package.json"), "utf8")) as { dependencies?: Record<string, string>; peerDependencies?: Record<string, string> };
const declared = [...Object.keys(pkg.dependencies ?? {}), ...Object.keys(pkg.peerDependencies ?? {})];
// a bare import is fine when it is a declared package or a path in one (react/jsx-runtime)
const allowed = (spec: string) => declared.some((d) => spec === d || spec.startsWith(`${d}/`));
// launcher-only files (relative to src/, a module or its declaration): Tauri's transport, the command layer and its
// mocks, the launcher's own pages, components and shell (but the server navigation the admin pages share), the admin
// pages' browser mock and test harness, and the file-dialog parts of the design system
const FORBIDDEN_FILE = /^(api\/(commands|adminTauri|mock|fallback)|components\/|pages\/|shell\/(?!serverNav\.)|admin\/(mock|harness)\/|ui\/(TaskProgress|DropZone|dialogFolder)\b)/;

const problems: string[] = [];
for (const f of ["admin-ui.js", "admin-ui.css", "types/admin/web.d.ts"]) if (!existsSync(join(dist, f))) problems.push(`missing ${f}`);
if (problems.length) finish();

// the bundle and every chunk split off it: their imports are declared packages, nothing of Tauri is in them, and the
// modules their source maps name are the package's own (no launcher-only file, no undeclared package bundled in)
for (const name of readdirSync(dist).filter((n) => n.endsWith(".js"))) {
  const js = readFileSync(join(dist, name), "utf8");
  if (/tauri/i.test(js)) problems.push(`${name} mentions Tauri`);
  for (const m of js.matchAll(/(?:^|[;\s}])(?:import|export)\s*(?:[^"';]*?from\s*)?["']([^"'./][^"']*)["']/gm)) if (!allowed(m[1])) problems.push(`${name} imports ${m[1]}, which packages/admin-ui/package.json does not declare`);
  const mapFile = join(dist, `${name}.map`);
  if (!existsSync(mapFile)) {
    problems.push(`${name} has no source map, so what it bundles cannot be checked`);
    continue;
  }
  const map = JSON.parse(readFileSync(mapFile, "utf8")) as { sourceRoot?: string; sources?: (string | null)[] };
  for (const s of map.sources ?? []) {
    if (!s || s.startsWith("\0")) continue; // the bundler's own helpers
    const rel = relative(here, resolve(dist, map.sourceRoot ?? "", s)).replaceAll("\\", "/");
    const nm = rel.lastIndexOf("node_modules/");
    if (nm >= 0) {
      const [scope, name2] = rel.slice(nm + "node_modules/".length).split("/");
      const dep = scope.startsWith("@") ? `${scope}/${name2}` : scope;
      if (!allowed(dep)) problems.push(`${name} bundles ${dep}, which packages/admin-ui/package.json does not declare`);
    } else if (rel.startsWith("src/") && FORBIDDEN_FILE.test(rel.slice("src/".length))) problems.push(`${name} bundles ${rel}`);
  }
}

// the declarations: what the entry reaches, without stylesheets, and nothing of the launcher's backend
const types = join(dist, "types");
const reached = new Set<string>();
const todo = [join(types, "admin", "web.d.ts")];
while (todo.length) {
  const f = todo.pop()!;
  if (reached.has(f)) continue;
  reached.add(f);
  let text = readFileSync(f, "utf8");
  const tidy = text.replace(/^import\s+["'][^"']+\.css["'];\r?\n/gm, "");
  if (tidy !== text) writeFileSync(f, (text = tidy));
  const rel = relative(types, f).replaceAll("\\", "/");
  if (FORBIDDEN_FILE.test(rel)) problems.push(`the declarations reach ${rel}`);
  for (const m of text.matchAll(/(?:from\s+|import\(\s*)["']([^"']+)["']/g)) {
    const spec = m[1];
    if (spec.startsWith(".")) {
      const base = resolve(dirname(f), spec);
      const hit = [`${base}.d.ts`, join(base, "index.d.ts")].find((p) => existsSync(p));
      if (hit) todo.push(hit);
      else problems.push(`${rel}: ${spec} has no declaration`);
    } else if (!allowed(spec)) problems.push(`${rel} imports ${spec}, which packages/admin-ui/package.json does not declare`);
  }
}
// declarations of modules the entry does not reach (tsc writes one per file of its program) are not the package's
const walk = (d: string): string[] => readdirSync(d).flatMap((n) => (statSync(join(d, n)).isDirectory() ? walk(join(d, n)) : [join(d, n)]));
let dropped = 0;
for (const f of walk(types)) if (!reached.has(f)) { rmSync(f); dropped++; }
finish();

function finish(): never {
  if (problems.length) {
    console.error(`@kiln/admin-ui: ${problems.length} problem(s) in ${relative(here, dist) || dist}:`);
    for (const p of problems) console.error(`  ${p}`);
    process.exit(1);
  }
  console.log(`@kiln/admin-ui: ok (${relative(here, dist)}: admin-ui.js, admin-ui.css, ${reached.size} declarations; ${dropped} unreached dropped)`);
  process.exit(0);
}
