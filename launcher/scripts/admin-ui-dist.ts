// Finishes and checks the admin pages' package build (@kiln/admin-ui): `bun run build:admin-ui` runs vite
// (vite.admin-ui.config.ts) and tsc (tsconfig.admin-ui.json), then this. It keeps the declarations the entry reaches
// (dist/types/admin/web.d.ts and what it imports), drops the stylesheet imports tsc copies into them (the styles are
// dist/admin-ui.css), and fails when the package reaches what a web page must never load: Tauri, the launcher's
// command layer or its mock, or a package packages/admin-ui/package.json does not declare.
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
const FORBIDDEN_FILE = /^(api\/(commands|adminTauri|mock|fallback)|components\/|pages\/|admin\/mock\/|ui\/(TaskProgress|DropZone|dialogFolder)\b)/;

const problems: string[] = [];
for (const f of ["admin-ui.js", "admin-ui.css", "types/admin/web.d.ts"]) if (!existsSync(join(dist, f))) problems.push(`missing ${f}`);
if (problems.length) finish();

// the bundle: its imports are declared packages, and nothing of Tauri is in it
const js = readFileSync(join(dist, "admin-ui.js"), "utf8");
if (/tauri/i.test(js)) problems.push("admin-ui.js mentions Tauri");
for (const m of js.matchAll(/(?:^|[;\s}])(?:import|export)\s*(?:[^"';]*?from\s*)?["']([^"'./][^"']*)["']/gm)) if (!allowed(m[1])) problems.push(`admin-ui.js imports ${m[1]}, which packages/admin-ui/package.json does not declare`);

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
