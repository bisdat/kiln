// Playwright smoke tests in mock mode: the vite dev server in a plain browser, so commands.ts
// answers from src/api/mock.ts.   bun run test:smoke
// Browser: Playwright's own chromium when installed (`bun x playwright install chromium`), else KILN_CHROMIUM or the
// newest chromium headless shell found in ~/.cache/ms-playwright (an older revision works for these tests).
// KILN_DEV_PORT runs (or reuses) the dev server on another port than 1420.
import { defineConfig } from "@playwright/test";
import { existsSync, readdirSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

function chromium(): string | undefined {
  if (process.env.KILN_CHROMIUM) return process.env.KILN_CHROMIUM;
  const cache = join(homedir(), ".cache", "ms-playwright");
  if (!existsSync(cache)) return undefined;
  const shells = readdirSync(cache).filter((d) => d.startsWith("chromium_headless_shell-")).sort().reverse();
  for (const s of shells) {
    const exe = join(cache, s, "chrome-headless-shell-linux64", "chrome-headless-shell");
    if (existsSync(exe)) return exe;
  }
  return undefined;
}

const PORT = Number(process.env.KILN_DEV_PORT || 1420);
// Each test has its own page (the mock's state lives in the page and its own storage), so tests run side by side:
// fullyParallel spreads the tests of one file over the workers too (a file that needs its order says
// `test.describe.configure({ mode: "serial" })`). Three workers by default, the slot limiter's job cap;
// KILN_PW_WORKERS sets another number (1 runs everything in order, as before).
const WORKERS = Number(process.env.KILN_PW_WORKERS || 3);

export default defineConfig({
  testDir: "tests",
  timeout: 60_000,
  // an expectation waits up to 10 s (Playwright's default is 5 s): on a loaded PC, with workers side by side, a page can
  // take longer than 5 s to show what it shows at once on an idle one; a passing check takes no longer for it
  expect: { timeout: 10_000 },
  workers: WORKERS,
  fullyParallel: true,
  reporter: [["list"]],
  use: {
    baseURL: `http://localhost:${PORT}`,
    viewport: { width: 1360, height: 860 },
    launchOptions: { executablePath: chromium() },
  },
  webServer: { command: PORT === 1420 ? "bun run dev" : `bun run dev --port ${PORT}`, url: `http://localhost:${PORT}`, reuseExistingServer: true },
});
