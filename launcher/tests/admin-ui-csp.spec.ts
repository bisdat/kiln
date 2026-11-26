// The admin pages (@kiln/admin-ui) under Kiln Web Admin's Content-Security-Policy: the harness (src/admin/harness, the
// package's entry from source) built for production as a host would build it, served from a fake origin with the mod's
// exact header on every response, over a fake KAP socket (./fixtures/kapServer). Every page the mod vendors is opened
// (the 13 admin pages and Configs); the guard fails on any CSP violation (an inline style attribute in HTML a page
// writes, a <style> element, an inline script, eval, a font or a socket from elsewhere) and on a page without exactly
// one h1. React's style={{...}} is not a violation: React writes it through the CSSOM (element.style), which CSP
// leaves alone; a style="..." in HTML (innerHTML, a Leaflet divIcon's html, setAttribute) is one.
// The dev server cannot be used here: it injects inline scripts and <style> elements itself.
import { expect, test, type Page } from "@playwright/test";
import react from "@vitejs/plugin-react";
import { mkdtempSync, readFileSync, rmSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { extname, join, normalize } from "node:path";
import { build } from "vite";
import { kapServer } from "./fixtures/kapServer";

/** Kiln Web Admin's header, as its server sends it (kiln-webadmin src/Kiln.WebAdmin.Core/Http/SecurityHeaders.cs). */
const CSP = "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data: blob:; font-src 'self'; connect-src 'self'; " +
  "worker-src 'self'; manifest-src 'self'; frame-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'";
const ORIGIN = "http://admin-csp.test";
const HARNESS = join(import.meta.dirname, "..", "src", "admin", "harness");
const TYPES: Record<string, string> = {
  ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".woff2": "font/woff2", ".woff": "font/woff", ".ttf": "font/ttf",
  ".svg": "image/svg+xml", ".png": "image/png", ".webp": "image/webp", ".jpg": "image/jpeg", ".json": "application/json",
};
const LOAD = { timeout: 30_000 };

/** The pages Kiln Web Admin mounts, with something each shows once it has its data. */
const PAGES: [string, (page: Page) => Promise<void>][] = [
  ["admin/players", async (p) => { await expect(p.getByText("Brannoc").first()).toBeVisible(LOAD); }],
  ["admin/map", async (p) => {
    await expect(p.getByRole("searchbox", { name: "Search the map" })).toBeVisible(LOAD);
    // the players' name labels with their health bars (the bar's width is the custom property --hp)
    await expect(p.locator(".ka-player em").first()).toBeAttached(LOAD);
    const hp = await p.locator(".ka-player em").evaluateAll((els) => els.map((e) => getComputedStyle(e).getPropertyValue("--hp").trim()));
    expect.soft(hp.length, "admin/map: player labels with a health bar").toBeGreaterThan(0);
    for (const v of hp) expect.soft(v, "admin/map: a health bar's --hp").toMatch(/^\d{1,3}%$/);
  }],
  ["admin/zones", async (p) => { await expect(p.getByText("Spawn village").first()).toBeVisible(LOAD); }],
  ["admin/items", async (p) => {
    await p.getByLabel("Search items").fill("Sword");
    await expect(p.getByRole("row").nth(1)).toBeVisible(LOAD);
  }],
  ["admin/spawn", async () => undefined],
  ["admin/world", async (p) => { await expect(p.getByRole("textbox", { name: "Broadcast text" })).toBeVisible(LOAD); }],
  ["admin/combat", async (p) => { await expect(p.getByText("Brannoc").first()).toBeVisible(LOAD); }],
  ["admin/creatures", async () => undefined],
  ["admin/gamedata", async () => undefined],
  ["admin/permissions", async () => undefined],
  ["admin/content", async () => undefined],
  ["admin/saves", async () => undefined],
  ["admin/crashes", async () => undefined],
  ["configs", async () => undefined],
];

let dist = "";

test.describe.configure({ mode: "default" }); // one build, the tests in order in one worker

test.beforeAll(async () => {
  test.setTimeout(240_000);
  dist = mkdtempSync(join(tmpdir(), "kiln-admin-csp-"));
  await build({
    configFile: false, root: HARNESS, base: "/", logLevel: "warn", publicDir: false, clearScreen: false,
    plugins: [react()],
    // as Kiln Web Admin builds it: assets are files (a data: font breaks font-src 'self'), no source maps
    build: { outDir: dist, emptyOutDir: true, target: "es2022", sourcemap: false, assetsInlineLimit: 0, chunkSizeWarningLimit: 5000 },
  });
});

test.afterAll(() => { if (dist) rmSync(dist, { recursive: true, force: true }); });

/** The built harness at ORIGIN, every response with the CSP; and every violation the page reports. */
async function serve(page: Page) {
  await page.route(`${ORIGIN}/**`, (route) => {
    const path = new URL(route.request().url()).pathname;
    const file = normalize(join(dist, path === "/" ? "index.html" : decodeURIComponent(path)));
    const headers = { "content-security-policy": CSP };
    if (!file.startsWith(dist) || !statSync(file, { throwIfNoEntry: false })?.isFile()) return route.fulfill({ status: 404, body: "not found", headers });
    return route.fulfill({ body: readFileSync(file), contentType: TYPES[extname(file)] ?? "application/octet-stream", headers });
  });
  // init scripts run outside the page's CSP: the listener is there before the first script
  await page.addInitScript(() => {
    const w = window as unknown as { __csp: string[] };
    w.__csp = [];
    document.addEventListener("securitypolicyviolation", (e) => {
      w.__csp.push(`${e.effectiveDirective} blocked ${e.blockedURI || "inline"} at ${e.sourceFile || "?"}:${e.lineNumber}${e.sample ? ` "${e.sample}"` : ""}`);
    });
  });
  const reported: string[] = [];
  page.on("console", (m) => { if (/Content Security Policy/i.test(m.text())) reported.push(`console: ${m.text()}`); });
  page.on("pageerror", (e) => reported.push(`pageerror: ${e.message}`));
  /** every violation so far: the page's reports, then the console's and the page errors */
  return { violations: async () => [...(await page.evaluate(() => (window as unknown as { __csp: string[] }).__csp)), ...reported] };
}

test("every admin page Kiln Web Admin vendors runs under its CSP with no violation and one h1", async ({ page }) => {
  test.setTimeout(180_000);
  const { violations } = await serve(page);
  await kapServer(page);
  await page.goto(`${ORIGIN}/index.html?kap=ws://admin-csp.test/kap#/servers/lab/admin/players`);
  const found: string[] = [];
  const counted = new Set<string>();
  for (const [route, ready] of PAGES) {
    await page.evaluate((r) => { location.hash = `#/servers/lab/${r}`; }, route);
    await ready(page);
    await page.waitForTimeout(1200); // a state push or two: live overlays redraw
    await expect.soft(page.getByRole("heading", { level: 1 }), `${route}: exactly one h1`).toHaveCount(1);
    for (const v of await violations()) if (!counted.has(v)) { counted.add(v); found.push(`${route}: ${v}`); }
  }
  expect(found).toEqual([]);
});

test("the guard sees an inline style attribute (the probe the test above relies on)", async ({ page }) => {
  const { violations } = await serve(page);
  await kapServer(page);
  await page.goto(`${ORIGIN}/index.html?kap=ws://admin-csp.test/kap#/servers/lab/admin/players`);
  await expect(page.getByText("Brannoc").first()).toBeVisible(LOAD);
  await page.evaluate(() => {
    const d = document.createElement("div");
    d.innerHTML = '<em style="--hp:50%"></em>';
    document.body.appendChild(d);
    const e = document.createElement("i");
    e.style.setProperty("--hp", "50%"); // the CSSOM: allowed
    document.body.appendChild(e);
  });
  await expect.poll(violations).toEqual([expect.stringMatching(/^style-src-attr blocked inline/), expect.stringMatching(/^console: .*Content Security Policy/)]);
});
