// EMF mods whose dependency is missing (kiln_manage::emf_deps; PepegaJedi's Mage Skills report, 0.2.0-dev.30.1): the main
// list's row says what is missing before Play and links where to get it, and Play's sentence names the mod. Browser
// mock with ?emfdeps=1: Ember run has Mage Skills (needs EMF's lua mod >=1.36) and Pet Tricks (needs pet-core ^2.0,
// whose mod.json entry gives its page) on, neither dependency installed.
import { expect, test, type Page } from "@playwright/test";

type Mock = { __kilnMock: { opened(): string[] } };
const list = (page: Page) => page.locator(".k-hero").getByRole("list", { name: "Mods" });
const row = (page: Page, name: string) => list(page).getByRole("listitem").filter({ has: page.getByText(name, { exact: true }) });
const say = (page: Page) => page.locator(".k-hero .k-hero-say");
const opened = (page: Page) => page.evaluate(() => (window as unknown as Mock).__kilnMock.opened());

test("an EMF mod whose dependency is not installed says so on its row, with how to get it", async ({ page }) => {
  await page.goto("/?emfdeps=1#/");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  const mage = row(page, "Mage Skills");
  await expect(mage).toContainText("Needs EMF's lua mod >=1.36, which isn't installed.", { timeout: 10_000 });
  // the details: the note's title has the hint and EMF's own words
  await expect(mage.getByText("Needs EMF's lua mod >=1.36, which isn't installed.")).toHaveAttribute("title",
    "Needs EMF's lua mod >=1.36, which isn't installed. It comes with EMF's release: add EMF's zip with Add mods. EMF says: needs lua, which is not installed.");
  await mage.getByRole("button", { name: "Get EMF" }).click();
  await expect.poll(() => opened(page)).toContain("https://github.com/matenm/emf-enshrouded");

  // another mod's dependency: named, with the page its mod.json gives, no EMF hint
  const pets = row(page, "Pet Tricks");
  await expect(pets).toContainText("Needs pet-core ^2.0, which isn't installed.");
  await expect(pets.getByRole("button", { name: "Get EMF" })).toHaveCount(0);
  await pets.getByRole("button", { name: "Mod page" }).click();
  await expect.poll(() => opened(page)).toContain("https://www.nexusmods.com/enshrouded/mods/9001");

  // Play's sentence counts them among the mods that will not run
  await expect(say(page)).toContainText("will not run in this game.");
  await expect(say(page)).toContainText("Mage Skills");

  // switched off, the row says nothing more
  await mage.getByRole("switch", { name: "Mage Skills" }).click();
  await expect(mage.getByRole("switch", { name: "Mage Skills" })).toHaveAttribute("aria-checked", "false");
  await expect(mage).not.toContainText("isn't installed");
  await expect(mage.getByRole("button", { name: "Get EMF" })).toHaveCount(0);
});

test("the load order lists the EMF mod among the mods that do not load, with the reason", async ({ page }) => {
  await page.goto("/?emfdeps=1#/mods/order");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  const item = page.getByRole("listitem").filter({ has: page.getByText("Mage Skills", { exact: true }) }).first();
  await expect(item).toBeVisible({ timeout: 10_000 });
  await expect(item.locator(".k-mod-note span")).toHaveAttribute("title", /^Needs EMF's lua mod >=1\.36, which isn't installed\. It comes with EMF's release/);
});
