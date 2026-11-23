// A short window keeps the mod list on Play (Somamint, 30.4, Steam Deck in desktop mode under KDE: about 720 px tall).
// The OptiScaler note and ShroudForge's own-loader notice above the list left it no rows and no scroll. Now each
// notice folds to its title line (Hide / Show, remembered across starts) and the list keeps three rows while the
// column scrolls.   bun x playwright test tests/play-short-window.spec.ts   (SHOTS_DIR=... keeps screenshots)
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";

const SHOTS = process.env.SHOTS_DIR;
const shot = async (page: Page, name: string) => {
  if (!SHOTS) return;
  mkdirSync(SHOTS, { recursive: true });
  await page.screenshot({ path: `${SHOTS}/${name}.png` });
};

test.use({ viewport: { width: 1280, height: 720 } });

const OPTI = "OptiScaler's own menu is off; Kiln's overlay draws next to it. FSR and XeSS come from OptiScaler while it is in the game folder, and Kiln's upscaler steps aside.";

async function open(page: Page) {
  await page.goto("/?sfSelf=started#/");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await page.evaluate((text) => (window as unknown as { __kilnMock: { setGraphicsTools: (t: unknown[]) => void } }).__kilnMock
    .setGraphicsTools([{ engine: "optiscaler", file: "dxgi.dll", text, kilnOverlay: true }]), OPTI);
}

test("with both notices the list still shows rows, and each notice folds and stays folded", async ({ page }) => {
  await open(page);
  const opti = page.locator(".k-banner").filter({ hasText: "OptiScaler" });
  const sf = page.locator(".k-banner").filter({ hasText: "ShroudForge's own loader is installed" });
  await expect(opti).toContainText(OPTI);
  await expect(sf).toContainText("(dbghelp.dll)");
  const list = page.locator(".k-mods-list");
  // three rows at least, even with both notices open
  expect((await list.boundingBox())!.height).toBeGreaterThanOrEqual(120);
  await list.scrollIntoViewIfNeeded();
  await expect(list.locator(".k-mod").first()).toBeInViewport();
  // nothing of the list runs over the facts under it
  const listBox = (await page.locator(".k-mods").boundingBox())!;
  const facts = page.locator(".k-hero-facts");
  if (await facts.count()) expect((await facts.boundingBox())!.y).toBeGreaterThanOrEqual(listBox.y + listBox.height - 1);
  await shot(page, "play-720-open");
  // fold both: their titles stay, their text goes, the column needs less room (the list keeps its rows)
  const hero = page.locator(".k-hero");
  const tall = () => hero.evaluate((el) => el.scrollHeight);
  const before = await tall();
  await opti.getByRole("button", { name: "Hide" }).click();
  await sf.getByRole("button", { name: "Hide" }).click();
  await expect(opti).not.toContainText(OPTI);
  await expect(sf).not.toContainText("Profile switches still update copies Kiln manages.");
  await expect(opti.getByRole("button", { name: "Show" })).toHaveAttribute("aria-expanded", "false");
  expect(await tall()).toBeLessThan(before);
  expect((await list.boundingBox())!.height).toBeGreaterThanOrEqual(120);
  await shot(page, "play-720-folded");
  // remembered after a restart of the page
  await open(page);
  await expect(page.locator(".k-banner").filter({ hasText: "OptiScaler" }).getByRole("button", { name: "Show" })).toBeVisible();
  await expect(page.locator(".k-banner").filter({ hasText: "ShroudForge's own loader is installed" })).not.toContainText("Profile switches still update copies Kiln manages.");
  // Show opens it again
  await page.locator(".k-banner").filter({ hasText: "OptiScaler" }).getByRole("button", { name: "Show" }).click();
  await expect(page.locator(".k-banner").filter({ hasText: "OptiScaler" })).toContainText(OPTI);
});
