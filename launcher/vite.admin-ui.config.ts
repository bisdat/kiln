// The admin pages' package build (@kiln/admin-ui, packages/admin-ui/package.json): src/admin/web.ts as an ES library in
// packages/admin-ui/dist (admin-ui.js, admin-ui.css); its declarations come from tsconfig.admin-ui.json, and
// scripts/admin-ui-dist.ts finishes and checks the result. `bun run build:admin-ui`. Not the launcher's build (vite.config.ts) and
// not the admin dev server (vite.admin.config.ts).
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

const root = fileURLToPath(new URL(".", import.meta.url));
const pkg = JSON.parse(readFileSync(`${root}packages/admin-ui/package.json`, "utf8")) as { dependencies?: Record<string, string>; peerDependencies?: Record<string, string> };
const deps = [...Object.keys(pkg.dependencies ?? {}), ...Object.keys(pkg.peerDependencies ?? {})];
// the package's dependencies stay imports (the host's bundler resolves them); their CSS (leaflet's) is bundled
const external = (id: string) => !id.endsWith(".css") && deps.some((d) => id === d || id.startsWith(`${d}/`));

export default defineConfig({
  root,
  plugins: [react()],
  publicDir: false,
  clearScreen: false,
  build: {
    outDir: "packages/admin-ui/dist",
    emptyOutDir: true,
    target: "es2022",
    sourcemap: true,
    cssCodeSplit: false,
    lib: { entry: "src/admin/web.ts", formats: ["es"], fileName: () => "admin-ui.js", cssFileName: "admin-ui" },
    rollupOptions: { external },
  },
});
