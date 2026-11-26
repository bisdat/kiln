// The Map page without map art says what its host says (configureAdmin's host.useMapNoArt): the banner over the map and
// the Markers tab's line. The launcher's words ("Preparing map art…", "Map art needs the Enshrouded client", "No map data
// yet: the launcher prepares it…") speak of its own Game art; Kilnwarden makes no art (its owner copies the launcher's)
// and says how instead. The host here is the test's, put where the web harness (src/admin/harness) reads it; the
// launcher itself (mock mode) keeps its own words.
import { expect, test, type Page } from "@playwright/test";
import { kapServer } from "./fixtures/kapServer";

const HARNESS = "/src/admin/harness/index.html?kap=ws://kap.test/kap#/servers/lab/admin/map";
const LOAD = { timeout: 30_000 };
const LAUNCHER_BANNER = /Preparing map art…|Map art needs the Enshrouded client/;
const LAUNCHER_MARKERS = /No map data yet: the launcher prepares it/;

/** No map art wherever the test runs (a checkout with launcher/assets-local has it): the map's probe finds nothing. */
async function withoutArt(page: Page) {
  await page.route("**/map/gpu/d/0_0.webp", (r) => r.fulfill({ status: 404, body: "" }));
}

/** A host as Kilnwarden's: the art's state says it makes none (canExtract false), and its own words when there is none;
 *  `banner: false` drops the banner over the map (the host shows its own notice). Counts the action's clicks and keeps
 *  the art states the hook was given. */
async function host(page: Page, banner: boolean) {
  await page.addInitScript((banner) => {
    const w = window as unknown as { __kilnHarnessHost: unknown; __noArtClicks: number; __noArtGiven: unknown[] };
    w.__noArtClicks = 0;
    w.__noArtGiven = [];
    w.__kilnHarnessHost = {
      useArtStatus: () => ({ map: false, extracted: null, canExtract: false }),
      useMapNoArt: (art: unknown) => {
        w.__noArtGiven.push(art);
        return banner
          ? {
            title: "This server has no map art yet",
            body: "The map shows players and zones on a plain background until it has.",
            action: { label: "Copy it from the Kiln Launcher", onClick: () => { w.__noArtClicks++; } },
            markers: "No markers without the map art: copy it from the Kiln Launcher (the notice over the map).",
          }
          : { title: null, markers: "No markers without the map art: see the notice above the map." };
      },
    };
  }, banner);
}

async function markersTab(page: Page) {
  await page.getByRole("tab", { name: "Markers", exact: true }).click();
  return page.locator(".ka-map-panel");
}

test("a host's words replace the banner over the map and the Markers tab's line", async ({ page }) => {
  await withoutArt(page);
  await host(page, true);
  await kapServer(page);
  await page.goto(HARNESS);
  const banner = page.locator(".ka-map-area .k-banner");
  await expect(banner).toBeVisible(LOAD);
  await expect(banner).toContainText("This server has no map art yet");
  await expect(banner).toContainText("The map shows players and zones on a plain background until it has.");
  await expect(page.locator(".ka-map-area")).not.toContainText(LAUNCHER_BANNER);
  await banner.getByRole("button", { name: "Copy it from the Kiln Launcher" }).click();
  await expect.poll(() => page.evaluate(() => (window as unknown as { __noArtClicks: number }).__noArtClicks)).toBe(1);
  // the hook gets the art's state as the page reads it (the host's useArtStatus)
  expect((await page.evaluate(() => (window as unknown as { __noArtGiven: unknown[] }).__noArtGiven)).at(-1))
    .toEqual({ map: false, extracted: null, canExtract: false });

  const panel = await markersTab(page);
  await expect(panel).toContainText("No markers without the map art: copy it from the Kiln Launcher (the notice over the map).");
  await expect(panel).not.toContainText(LAUNCHER_MARKERS);
});

test("a host may drop the banner over the map (its own notice says it) and still words the Markers tab", async ({ page }) => {
  await withoutArt(page);
  await host(page, false);
  await kapServer(page);
  await page.goto(HARNESS);
  await expect(page.locator(".ka-map")).toBeVisible(LOAD);
  const panel = await markersTab(page);
  await expect(panel).toContainText("No markers without the map art: see the notice above the map.");
  await expect(panel).not.toContainText(LAUNCHER_MARKERS);
  await expect(page.locator(".ka-map-area .k-banner")).toHaveCount(0);
  await expect(page.locator(".ka-map-area")).not.toContainText(LAUNCHER_BANNER);
});

test("the launcher, without the hook, keeps its own words", async ({ page }) => {
  await withoutArt(page);
  await page.goto("/#/servers/lab/admin/map");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  if (await page.evaluate(() => location.hash !== "#/servers/lab/admin/map")) await page.evaluate(() => { location.hash = "#/servers/lab/admin/map"; });
  const banner = page.locator(".ka-map-area .k-banner");
  await expect(banner).toBeVisible(LOAD);
  await expect(banner).toHaveText(LAUNCHER_BANNER);
  const panel = await markersTab(page);
  await expect(panel).toContainText("No map data yet: the launcher prepares it from your installed game (Preparing game art in the top bar).");
});
