// Settings (mockup A, "Section list"; pages/SettingsPage.tsx, pages/settings/*): six sections down the left with a live
// line each, one section at a time on the right; search over every setting's name, help and More text with working
// controls under their place, "Open in <section>", counts per section and Enter that jumps; every deep link in use opens
// its section and flushes its group with ember; /settings/<section> routes; the 1280×800 and 1024×640 windows.
// SETTINGS_SHOTS=<dir> writes a screenshot of each section at 1440×900 and 1280×800 there.
import { expect, test, type Locator, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";
import { join } from "node:path";

type Mock = { __kilnMock: { kilnJsonValues: () => { graphics?: Record<string, unknown> } } };
const SECTIONS = [
  ["general", "General"], ["mods", "Mods & profiles"], ["game", "Game & Kiln"], ["graphics", "Graphics"], ["online", "Updates & online"], ["advanced", "Advanced"],
] as const;

async function open(page: Page, route: string, query = "") {
  await page.goto(`/${query}#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}
const nav = (page: Page) => page.getByRole("navigation", { name: "Settings sections" });
const heading = (page: Page) => page.locator(".k-st-title");
const box = (page: Page) => page.getByRole("textbox", { name: "Search" });
/** True while the element's box is inside the scrolling column. */
async function inView(page: Page, el: Locator) {
  const main = await page.locator(".k-st-main").boundingBox();
  const b = await el.boundingBox();
  return !!main && !!b && b.y >= main.y - 1 && b.y < main.y + main.height;
}

test("six sections with a live line each; one shows at a time with its groups as links", async ({ page }) => {
  await open(page, "/settings");
  await expect(heading(page)).toHaveText("General");
  const links = nav(page).getByRole("link");
  await expect(links).toHaveCount(6);
  for (const [, name] of SECTIONS) await expect(nav(page).getByRole("link", { name, exact: true })).toBeVisible();
  await expect(nav(page).getByRole("link", { name: "General", exact: true })).toHaveAttribute("aria-current", "page");
  // the live lines: what each section is set to
  await expect(nav(page).locator('[data-section="general"] .k-st-sec-line')).toHaveText("English · fire on · Deck: Auto");
  await expect(nav(page).locator('[data-section="mods"] .k-st-sec-line')).toHaveText("Ember run loaded · 14 mods");
  await expect(nav(page).locator('[data-section="game"] .k-st-sec-line')).toContainText("Enshrouded · Kiln 0.2.0");
  await expect(nav(page).locator('[data-section="graphics"] .k-st-sec-line')).toHaveText("Game's FSR 3 · no frame cap");
  await expect(nav(page).locator('[data-section="online"] .k-st-sec-line')).toContainText("Up to date");
  await expect(nav(page).locator('[data-section="advanced"] .k-st-sec-line')).toHaveText("Drawing, sandbox, kiln.json");
  // the launcher's version at the bottom of the list
  const version = await page.evaluate(async () => (await (await import("/src/api/commands.ts")).appInfo()).version);
  await expect(page.locator(".k-st-foot")).toHaveText(version);
  // General's groups as links under it
  const subs = nav(page).getByRole("list", { name: "Groups of General" }).getByRole("button");
  await expect(subs).toHaveText(["Language", "Look", "Steam Deck", "On this computer"]);

  // another section: its own route, its own groups, nothing of General's
  await nav(page).getByRole("link", { name: "Graphics", exact: true }).click();
  await expect(page).toHaveURL(/#\/settings\/graphics$/);
  await expect(heading(page)).toHaveText("Graphics");
  await expect(page.locator(".k-st-say")).toHaveText("Kiln's frame cap, DLSS settings and upscaler for Enshrouded.");
  await expect(nav(page).getByRole("link", { name: "Graphics", exact: true })).toHaveAttribute("aria-current", "page");
  await expect(page.getByRole("region", { name: "Language" })).toHaveCount(0);
  await expect(nav(page).getByRole("list", { name: "Groups of Graphics" }).getByRole("button")).toHaveText(["Kiln's graphics tools", "Behind the game's FSR 3", "Upscaler files"]);
  // a group's link scrolls its group to the top of the column
  await nav(page).getByRole("button", { name: "Upscaler files" }).click();
  const files = page.getByRole("region", { name: "Upscaler files" });
  await expect.poll(() => inView(page, files)).toBe(true);
  await expect(nav(page).getByRole("button", { name: "Upscaler files" })).toHaveAttribute("aria-current", "true");

  // every section by its route
  for (const [id, name] of SECTIONS) {
    await open(page, `/settings/${id}`);
    await expect(heading(page), id).toHaveText(name);
  }
});

test("/settings opens the section last shown in this run; More opens the rest of a row's help", async ({ page }) => {
  await open(page, "/settings/advanced");
  await expect(heading(page)).toHaveText("Advanced");
  await page.getByRole("navigation", { name: "Main" }).getByRole("link", { name: "Play" }).click();
  await page.getByRole("link", { name: "Settings", exact: true }).click();
  await expect(heading(page)).toHaveText("Advanced");
  // one line of help, the rest under More (today's whole text); NEXT START beside the name
  const row = page.locator('[data-setting="sandbox"]');
  await expect(row.locator(".k-st-when")).toHaveText("next start");
  await expect(row).toContainText("Keep it on unless the window will not open.");
  await expect(row).not.toContainText("Applies at the next start. Off: the launcher's page runs without WebKit's process sandbox");
  await row.getByRole("button", { name: "More" }).click();
  await expect(row).toContainText("Applies at the next start. Off: the launcher's page runs without WebKit's process sandbox");
  await row.getByRole("button", { name: "Less" }).click();
  await expect(row.getByRole("button", { name: "More" })).toHaveAttribute("aria-expanded", "false");
});

test("search: hits by section with counts, a change in place, Open in, Enter jumps and flushes the row, Esc clears", async ({ page }) => {
  await open(page, "/settings");
  // Ctrl+F finds the box
  await page.keyboard.press("Control+f");
  await expect(box(page)).toBeFocused();
  await page.keyboard.type("upscaler");
  await expect(heading(page)).toHaveText("Search");
  await expect(page.locator(".k-st-say")).toContainText("match “upscaler”. All in Graphics.");
  // the counts: Graphics has them all, the others are dimmed
  const graphics = nav(page).locator('[data-section="graphics"]');
  await expect(graphics.locator(".k-st-count")).toHaveText(/^\d+$/);
  await expect(graphics.locator(".k-st-sec-line")).toContainText("Behind the game's FSR 3");
  for (const s of ["general", "mods", "game", "online", "advanced"]) {
    await expect(nav(page).locator(`li.dim > [data-section="${s}"]`), s).toHaveCount(1);
    await expect(nav(page).locator(`[data-section="${s}"] .k-st-count`), s).toHaveCount(0);
  }
  // the hits are the real controls, under their place: change the upscaler right here
  const choice = page.getByRole("region", { name: "Behind the game's FSR 3" });
  await expect(choice.locator(".k-st-crumb")).toContainText("Graphics ›");
  await expect(choice.locator("mark").first()).toHaveText(/upscaler/i);
  await choice.getByLabel("Upscaler", { exact: true }).selectOption("amd");
  await expect.poll(() => page.evaluate(() => (window as unknown as Mock).__kilnMock.kilnJsonValues().graphics?.upscaler)).toBe("amd");
  await expect(page.getByRole("region", { name: "Upscaler files" })).toBeVisible();
  // a word only the More text has: the hit quotes it
  await box(page).fill("weak GPUs");
  const fire = page.locator('[data-setting="fire"]');
  await expect(fire.locator(".k-st-quote")).toContainText("easier on weak GPUs");
  await expect(nav(page).locator('[data-section="general"] .k-st-count')).toHaveText("1");
  // Open in General: the section, the row in view, flushed with ember
  await page.getByRole("button", { name: "Open in General" }).click();
  await expect(heading(page)).toHaveText("General");
  await expect(box(page)).toHaveValue("");
  await expect(page.locator('[data-setting="fire"]')).toHaveClass(/k-st-flush/);
  // Enter opens the first hit, in its section, and flushes its row; the flush goes after 2 s
  await box(page).fill("http modpacks");
  await expect(nav(page).locator('[data-section="advanced"] .k-st-count')).toHaveText("1");
  await box(page).press("Enter");
  await expect(page).toHaveURL(/#\/settings\/advanced$/);
  const row = page.locator('[data-setting="httpModpacks"]');
  await expect(row).toHaveClass(/k-st-flush/);
  await expect.poll(() => inView(page, row)).toBe(true);
  await expect(row).not.toHaveClass(/k-st-flush/, { timeout: 4000 });
  // Enter prefers a hit by its name ("Upscaler"), not the first in the page (Readout's help names the upscaler)
  await box(page).fill("upscaler");
  await box(page).press("Enter");
  await expect(page).toHaveURL(/#\/settings\/graphics$/);
  await expect(page.locator('[data-setting="upscaler.choice"]')).toHaveClass(/k-st-flush/);
  // a kiln.json entry's row: SchemaForm's
  await box(page).fill("graphics.readout");
  await box(page).press("Enter");
  const readout = page.locator(".k-sform-row", { has: page.locator("#sf-graphics-readout") });
  await expect(readout).toHaveClass(/k-st-flush/);
  await expect.poll(() => inView(page, readout)).toBe(true);
  // Esc clears; nothing found says so
  await box(page).fill("zzzz nothing");
  await expect(page.locator(".k-st-say")).toHaveText("Nothing matches “zzzz nothing”.");
  await box(page).press("Escape");
  await expect(box(page)).toHaveValue("");
  await expect(heading(page)).toHaveText("Graphics");
});

test("search reads the launcher's language", async ({ page }) => {
  await open(page, "/settings");
  await page.locator("#st-lang").selectOption("de");
  await expect.poll(() => page.evaluate(() => document.documentElement.lang)).toBe("de");
  // German names and More texts find their rows; the English name no longer does
  await page.locator(".k-st-rail input").fill("Animiertes Feuer");
  await expect(page.locator('[data-setting="fire"]')).toBeVisible();
  await page.locator(".k-st-rail input").fill("schwache GPUs");
  await expect(page.locator('[data-setting="fire"] .k-st-quote')).toContainText("schwache GPUs");
  await page.locator(".k-st-rail input").fill("Animated fire");
  await expect(page.locator('[data-setting="fire"]')).toHaveCount(0);
  // a kiln.json key finds its row in any language
  await page.locator(".k-st-rail input").fill("graphics.fpsCap");
  await expect(page.locator('[data-group="graphics"]')).toBeVisible();
  await page.locator(".k-st-rail input").fill("");
  await page.locator("#st-lang").selectOption("en");
  await expect.poll(() => page.evaluate(() => document.documentElement.lang)).toBe("en");
});

// every deep link the launcher has (grep "settings#"): its section opens, its group is flushed and in view
const LINKS: [string, string, string][] = [
  ["profiles", "Mods & profiles", "profiles"], ["crucible", "Mods & profiles", "crucible"], ["library", "Mods & profiles", "library"],
  ["nexus", "Updates & online", "nexus"], ["graphics", "Graphics", "graphics"], ["upscalers", "Graphics", "upscalers"],
  ["shroudforge", "Game & Kiln", "shroudforge"], ["language", "General", "language"], ["kilnjson", "Advanced", "kilnjson"],
];
for (const [hash, section, group] of LINKS) {
  test(`/settings#${hash} opens ${section} at its group`, async ({ page }) => {
    await open(page, `/settings#${hash}`);
    await expect(heading(page)).toHaveText(section);
    await expect(nav(page).getByRole("link", { name: section, exact: true })).toHaveAttribute("aria-current", "page");
    const g = page.locator(`[data-group="${group}"]`);
    await expect(g).toHaveClass(/k-st-flush/);
    await expect.poll(() => inView(page, g)).toBe(true);
    // the URL stays as it was linked
    await expect(page).toHaveURL(new RegExp(`#/settings#${hash}$`));
  });
}

test("the links into Settings land on their group: Play's profile line, the catalog's sign-in, an old /profiles route", async ({ page }) => {
  await open(page, "/");
  await page.locator(".k-hero").getByRole("link", { name: /Ember run/ }).click();
  await expect(heading(page)).toHaveText("Mods & profiles");
  await expect(page.locator('[data-group="profiles"]')).toHaveClass(/k-st-flush/);
  await open(page, "/catalog", "?nexus=signedout");
  await page.getByRole("button", { name: "Settings", exact: true }).first().click();
  await expect(heading(page)).toHaveText("Updates & online");
  await expect(page.locator('[data-group="nexus"]')).toHaveClass(/k-st-flush/);
  await open(page, "/profiles/vanilla-plus/mods");
  await expect(page).toHaveURL(/#\/settings#profiles$/);
  await expect(heading(page)).toHaveText("Mods & profiles");
});

for (const [w, h] of [[1280, 800], [1024, 640]] as const) {
  test(`every section fits ${w}×${h}: the list whole, the column scrolls`, async ({ page }) => {
    await page.setViewportSize({ width: w, height: h });
    for (const [id, name] of SECTIONS) {
      await open(page, `/settings/${id}`);
      await expect(heading(page)).toHaveText(name);
      await page.waitForTimeout(300);
      const over = await page.evaluate(() => ({ x: document.documentElement.scrollWidth > innerWidth + 1, y: document.documentElement.scrollHeight > innerHeight + 1 }));
      expect(over, id).toEqual({ x: false, y: false });
      // all six sections can be reached in the list, and the version under them
      for (const [, n] of SECTIONS) {
        const link = nav(page).getByRole("link", { name: n, exact: true });
        await link.scrollIntoViewIfNeeded();
        await expect(link, `${id}: ${n}`).toBeInViewport();
      }
      // the column scrolls to its last group's end, inside the window
      const end = await page.locator(".k-st-main").evaluate((el) => { el.scrollTop = el.scrollHeight; return el.getBoundingClientRect().bottom; });
      expect(end, id).toBeLessThanOrEqual(h);
      const last = page.locator(".k-st-body > *").last();
      await expect(last, id).toBeInViewport();
    }
    // the live lines go at the minimum (1024×640), and stay at 1280×800
    await expect(nav(page).locator(".k-st-sec-line").first()).toBeVisible({ visible: w > 1100 });
  });
}

test("screenshots of each section at 1440×900 and 1280×800", async ({ page }) => {
  const dir = process.env.SETTINGS_SHOTS;
  test.skip(!dir, "SETTINGS_SHOTS=<dir> writes them");
  test.setTimeout(120_000);
  mkdirSync(dir!, { recursive: true });
  for (const [w, h] of [[1440, 900], [1280, 800]]) {
    await page.setViewportSize({ width: w, height: h });
    for (const [id] of SECTIONS) {
      await open(page, `/settings/${id}`);
      await page.waitForTimeout(900);
      await page.screenshot({ path: join(dir!, `settings-${id}-${w}x${h}.png`) });
    }
    await open(page, "/settings/general");
    await box(page).fill("upscaler");
    await page.waitForTimeout(700);
    await page.screenshot({ path: join(dir!, `settings-search-${w}x${h}.png`) });
    await box(page).press("Enter");
    await page.waitForTimeout(500);
    await page.screenshot({ path: join(dir!, `settings-jump-${w}x${h}.png`) });
  }
});
