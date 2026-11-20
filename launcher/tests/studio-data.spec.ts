// Pack Studio, Data (src/studio/data/; DESIGN 3.3) in mock mode: each editor writes its content/*.json section of the
// pack layer (checked through __kilnStudioMock.state()), the pickers find game entries and what the pack's mods add
// (Cinderbrand_Sword from New Items), the lint line follows every write (a selector that matches nothing: a warning;
// broken JSON: an error), Raw JSON makes, renames and deletes files. KILN_STUDIO_SHOTS=<dir> saves screenshots there.
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";

const SHOTS = process.env.KILN_STUDIO_SHOTS;
const shot = async (page: Page, name: string) => { if (SHOTS) { mkdirSync(SHOTS, { recursive: true }); await page.screenshot({ path: `${SHOTS}/${name}.png`, fullPage: true }); } };
type Layer = Record<string, string>;
const layer = (page: Page) => page.evaluate(() => (window as unknown as { __kilnStudioMock: { state: () => { source: { id: string }; layer: Layer }[] } }).__kilnStudioMock.state()
  .find((s) => s.source.id === "ember-run")!.layer);
const fileOf = async (page: Page, path: string) => { const l = await layer(page); return l[path] === undefined ? undefined : JSON.parse(l[path]); };
const requests = (page: Page) => page.evaluate(() => (window as unknown as { __kilnStudioMock: { requests: () => { cmd: string; args: Record<string, unknown> }[] } }).__kilnStudioMock.requests());

/** Opens a URL's Data tab. StudioPage sends a deep link to Play while crucible_status has not answered yet (useCrucibleOn
 *  is false until then), so the page first loads at Play and moves to the Studio once its nav link shows. */
async function goData(page: Page, query = "") {
  await page.goto("/?mock=1" + query + "#/");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.getByRole("link", { name: "Studio", exact: true })).toBeVisible();
  await page.evaluate(() => { location.hash = "#/studio/ember-run/data"; });
}
async function open(page: Page, sub?: string) {
  await goData(page);
  // the Data tab is a lazy chunk: a fresh dev server compiles it at the first visit (slow on a cold runner)
  await expect(page.getByTestId("studio-data")).toBeVisible({ timeout: 45_000 });
  await expect(page.getByTestId("data-lint")).toHaveText("Lint: 0 errors, 0 warnings");
  if (sub) await page.getByTestId("data-sub-" + sub).click();
}
/** Types into a picker and chooses the hit with that name. */
async function pick(page: Page, testId: string, query: string, name: string) {
  const input = page.getByTestId(testId + "-input");
  await input.fill(query);
  const hit = page.getByTestId(testId + "-hit-" + name);
  await expect(hit).toBeVisible();
  await hit.click();
}

test("Items: a rule for every material (x4, max 9999, raise only) next to the creator's rule; a selector matching nothing warns", async ({ page }) => {
  await open(page);
  // the seeded layer's rule shows with its picker (icon, display name) and typed value
  await expect(page.getByTestId("data-items-0-sel-input")).toHaveValue("Material_T1_Wood_Logs");
  await expect(page.getByTestId("data-items-0-sel-name")).toHaveText("Wood Logs");
  await expect(page.getByTestId("data-items-0-f0-n")).toHaveValue("500");
  await page.getByTestId("data-items-add-all").click();
  await expect(page.getByTestId("data-items-1-sel-input")).toHaveValue("*");
  await expect(page.getByTestId("data-items-1-f0-mode")).toHaveValue("mul");
  await expect(page.getByTestId("data-items-1-f0-raise")).toBeChecked();
  // the picker lists the game's items by display name
  await page.getByTestId("data-items-add").click();
  await pick(page, "data-items-2-sel", "spores", "Material_T2_Shroud_Spores");
  await page.getByTestId("data-items-2-f0-name").selectOption("rarity");
  await page.getByTestId("data-items-2-f0").selectOption("Epic");
  await expect(page.getByTestId("data-items-state")).toHaveText("Unsaved changes");
  await shot(page, "data-items");
  await page.getByTestId("data-items-save").click();
  await expect(page.getByTestId("data-items-state")).toHaveText("Saved");
  expect(await fileOf(page, "content/items.json")).toEqual({
    format: 1,
    items: {
      Material_T1_Wood_Logs: { maxStackSize: 500 },
      "*": { "@where": { category: "Materials" }, maxStackSize: { mul: 4, max: 9999, raiseOnly: true } },
      Material_T2_Shroud_Spores: { rarity: "Epic" },
    },
  });
  await expect(page.getByTestId("data-lint")).toHaveText("Lint: 0 errors, 0 warnings");
  // a name that is neither the game's nor a mod's: a warning, listed
  await page.getByTestId("data-items-add").click();
  await page.getByTestId("data-items-3-sel-input").fill("Not_A_Real_Item");
  await page.getByTestId("data-items-3-sel-input").press("Enter");
  await page.getByTestId("data-items-save").click();
  await expect(page.getByTestId("data-lint")).toHaveText("Lint: 0 errors, 1 warning");
  await page.getByTestId("data-lint-toggle").click();
  await expect(page.getByTestId("data-lint-list")).toContainText("no entry Not_A_Real_Item");
  await shot(page, "data-items-lint");
  // a write answers the lint (no extra lint call needed)
  const writes = (await requests(page)).filter((r) => r.cmd === "studio_layer_write");
  expect(writes.map((w) => w.args.path)).toEqual(["content/items.json", "content/items.json"]);
});

test("Recipes: change the mod's recipe (game and pack-mod items, station, duration) and add a recipe of the pack", async ({ page }) => {
  await open(page, "recipes");
  await page.getByTestId("data-recipes-add").click();
  // the recipe picker lists what New Items adds
  await pick(page, "data-recipes-rule-0-sel", "cinder", "Recipe_Cinderbrand_Sword");
  await expect(page.getByTestId("data-recipes-rule-0-note")).toHaveText("Changes the mod's recipe; the pack layer wins (it loads after New Items).");
  await page.getByTestId("data-recipes-rule-0-in-on").check();
  await pick(page, "data-recipes-rule-0-in-0-item", "spores", "Material_T2_Shroud_Spores");
  await page.getByTestId("data-recipes-rule-0-in-0-count").fill("4");
  await page.getByTestId("data-recipes-rule-0-in-0-count").press("Enter");
  await page.getByTestId("data-recipes-rule-0-in-add").click();
  // the pack's mods' own items, with where they come from
  await page.getByTestId("data-recipes-rule-0-in-1-item-input").fill("brass");
  await expect(page.getByTestId("data-recipes-rule-0-in-1-item-hit-Brass_Gear")).toContainText("New Items");
  await page.getByTestId("data-recipes-rule-0-in-1-item-hit-Brass_Gear").click();
  await page.getByTestId("data-recipes-rule-0-in-1-count").fill("2");
  await page.getByTestId("data-recipes-rule-0-duration").fill("30");
  await page.getByTestId("data-recipes-rule-0-duration").press("Enter");
  // a new recipe: station from the catalog, output the mod's Cinderbrand Sword
  await page.getByTestId("data-recipes-new").click();
  await page.getByTestId("data-recipes-new-0-name").fill("Ember_Run_Sword_Recipe");
  await pick(page, "data-recipes-new-0-station", "black", "Workshop_Blacksmith");
  await expect(page.getByTestId("data-recipes-new-0-station-input")).toHaveValue("0x0c0c0001");
  await pick(page, "data-recipes-new-0-in-0-item", "iron", "Material_T3_Iron_Bar");
  await page.getByTestId("data-recipes-new-0-in-0-count").fill("3");
  await page.getByTestId("data-recipes-new-0-in-0-count").press("Enter");
  await page.getByTestId("data-recipes-new-0-out-0-item-input").fill("cinder");
  const own = page.getByTestId("data-recipes-new-0-out-0-item-hit-Cinderbrand_Sword");
  await expect(own).toContainText("Cinderbrand Sword");
  await expect(own).toContainText("New Items");
  await shot(page, "data-recipes-picker");
  await own.click();
  await page.getByTestId("data-recipes-new-0-duration").fill("45");
  await page.getByTestId("data-recipes-new-0-duration").press("Enter");
  await shot(page, "data-recipes");
  await page.getByTestId("data-recipes-save").click();
  await expect(page.getByTestId("data-recipes-state")).toHaveText("Saved");
  const doc = await fileOf(page, "content/recipes.json");
  expect(doc.format).toBe(1);
  expect(doc.recipes.Recipe_Cinderbrand_Sword).toEqual({
    inputs: [{ item: "Material_T2_Shroud_Spores", count: 4 }, { item: "Brass_Gear", count: 2 }], craftingDuration: 30,
  });
  expect(doc.recipes.add).toHaveLength(1);
  expect(doc.recipes.add[0]).toMatchObject({
    name: "Ember_Run_Sword_Recipe", station: "0x0c0c0001", inputs: [{ item: "Material_T3_Iron_Bar", count: 3 }], outputs: [{ item: "Cinderbrand_Sword", count: 1 }], duration: 45,
  });
  expect(doc.recipes.add[0].guid).toMatch(/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/);
  await expect(page.getByTestId("data-lint")).toHaveText("Lint: 0 errors, 0 warnings");
});

test("Loot and Drops: stack sizes per item; a drop table by its GUID with chance and one item's counts", async ({ page }) => {
  await open(page, "loot");
  await page.getByTestId("data-loot-add").click();
  await pick(page, "data-loot-0-sel", "stone", "Material_T1_Stone");
  await expect(page.getByTestId("data-loot-0-f0-name")).toHaveValue("stackSizeMin");
  await page.getByTestId("data-loot-0-f1-n").fill("20");
  await page.getByTestId("data-loot-0-f1-n").press("Enter");
  await shot(page, "data-loot");
  await page.getByTestId("data-loot-save").click();
  await expect(page.getByTestId("data-loot-state")).toHaveText("Saved");
  expect(await fileOf(page, "content/loot.json")).toEqual({ format: 1, loot: { Material_T1_Stone: { stackSizeMin: 1, stackSizeMax: 20 } } });

  await page.getByTestId("data-sub-drops").click();
  await page.getByTestId("data-drops-add").click();
  await pick(page, "data-drops-0-sel", "scav", "Drop_Enemy_Scavenger");
  await expect(page.getByTestId("data-drops-0-sel-input")).toHaveValue("a20e8eae-7833-462e-9ea9-59d1e17a1b98");
  await expect(page.getByTestId("data-drops-0-sel-name")).toHaveText("Scavenger");
  await page.getByTestId("data-drops-0-chance-on").check();
  await page.getByTestId("data-drops-0-additem").click();
  await pick(page, "data-drops-0-item-0-sel", "scraps", "Material_T2_Metal_Scraps");
  await page.getByTestId("data-drops-0-item-0-f0-n").fill("10");
  await page.getByTestId("data-drops-0-item-0-f0-n").press("Enter");
  await page.getByTestId("data-drops-add-all").click();
  await shot(page, "data-drops");
  await page.getByTestId("data-drops-save").click();
  await expect(page.getByTestId("data-drops-state")).toHaveText("Saved");
  expect(await fileOf(page, "content/drops.json")).toEqual({
    format: 1,
    drops: {
      "a20e8eae-7833-462e-9ea9-59d1e17a1b98": { chance: { mul: 1.5, max: 1 }, items: { Material_T2_Metal_Scraps: { countMax: 10 } } },
      "*": { count: { mul: 2 } },
    },
  });
  await expect(page.getByTestId("data-lint")).toHaveText("Lint: 0 errors, 0 warnings");
});

test("New items: an add clone of a game item, kept beside the item rules; the pickers then offer it as the pack's own", async ({ page }) => {
  await open(page, "added");
  await page.getByTestId("data-added-add").click();
  await page.getByTestId("data-added-0-name").fill("Ember_Frost_Blade");
  await page.getByTestId("data-added-0-display").fill("Frost Blade");
  await pick(page, "data-added-0-base", "crafted sword", "Weapon_T1_1H_Sword_Craft");
  await page.getByTestId("data-added-0-addfield").click();
  await page.getByTestId("data-added-0-f0-name").selectOption("rarity");
  await page.getByTestId("data-added-0-f0").selectOption("Rare");
  await page.getByTestId("data-added-0-rolled").check();
  await shot(page, "data-added");
  await page.getByTestId("data-added-save").click();
  await expect(page.getByTestId("data-added-state")).toHaveText("Saved");
  const doc = await fileOf(page, "content/items.json");
  expect(Object.keys(doc.items)).toEqual(["add", "Material_T1_Wood_Logs"]);
  expect(doc.items.Material_T1_Wood_Logs).toEqual({ maxStackSize: 500 });
  expect(doc.items.add).toHaveLength(1);
  expect(doc.items.add[0]).toMatchObject({ name: "Ember_Frost_Blade", base: "Weapon_T1_1H_Sword_Craft", displayName: "Frost Blade", fields: { rarity: "Rare" }, rolledLoot: true });
  // Items: the new item is in the picker, marked as this pack's
  await page.getByTestId("data-sub-items").click();
  await page.getByTestId("data-items-add").click();
  await page.getByTestId("data-items-1-sel-input").fill("frost");
  await expect(page.getByTestId("data-items-1-sel-hit-Ember_Frost_Blade")).toContainText("this pack");
  await page.getByTestId("data-items-1-sel-hit-Ember_Frost_Blade").click();
  await page.getByTestId("data-items-save").click();
  await expect(page.getByTestId("data-items-state")).toHaveText("Saved");
  const after = await fileOf(page, "content/items.json");
  expect(after.items.add[0].name).toBe("Ember_Frost_Blade");
  expect(after.items.Ember_Frost_Blade).toEqual({ maxStackSize: 999 });
  await expect(page.getByTestId("data-lint")).toHaveText("Lint: 0 errors, 0 warnings");
});

test("Raw JSON: make a file, break it (an error), fix it, rename it, delete it", async ({ page }) => {
  await open(page, "raw");
  await expect(page.getByTestId("data-raw-text")).toHaveValue(/Material_T1_Wood_Logs/);
  await page.getByTestId("data-raw-new-name").fill("extras");
  await page.getByTestId("data-raw-new").click();
  await expect(page.getByTestId("data-raw-path")).toHaveText("content/extras.json");
  expect(JSON.parse((await layer(page))["content/extras.json"])).toEqual({ format: 1 });
  await page.getByTestId("data-raw-text").fill('{ "format": 1, "items": { "Material_T1_Twigs": { "maxStackSize": 50 } ');
  await page.getByTestId("data-raw-save").click();
  await expect(page.getByTestId("data-lint")).toHaveText("Lint: 1 error, 0 warnings");
  await expect(page.getByTestId("data-raw-lint")).toContainText("Not valid JSON");
  await shot(page, "data-raw-error");
  await page.getByTestId("data-raw-text").fill('{ "format": 1, "items": { "Material_T1_Twigs": { "maxStackSize": 50 } } }\n');
  await page.getByTestId("data-raw-save").click();
  await expect(page.getByTestId("data-lint")).toHaveText("Lint: 0 errors, 0 warnings");
  await expect(page.getByTestId("data-raw-lint")).toHaveText("No problems in this file.");
  // the Items editor says the section is also set there
  await page.getByTestId("data-sub-items").click();
  await expect(page.getByTestId("data-also")).toContainText("content/extras.json");
  await page.getByTestId("data-sub-raw").click();
  await page.getByTestId("data-raw-file-content/extras.json").click();
  await page.getByTestId("data-raw-rename").click();
  await page.getByTestId("data-raw-rename-name").fill("twigs.json");
  await page.getByTestId("data-raw-rename-ok").click();
  await expect(page.getByTestId("data-raw-path")).toHaveText("content/twigs.json");
  let files = Object.keys(await layer(page)).sort();
  expect(files).toEqual(["content/items.json", "content/twigs.json"]);
  await shot(page, "data-raw");
  await page.getByTestId("data-raw-delete").click();
  const dialog = page.getByRole("dialog");
  await expect(dialog).toBeVisible();
  // a typed confirmation only when the player turned it on (Settings); a hidden input is not one
  const word = dialog.getByRole("textbox");
  if (await word.count()) await word.first().fill("twigs.json");
  await dialog.getByRole("button", { name: "Delete" }).click();
  await expect(page.getByTestId("data-raw-file-content/twigs.json")).toHaveCount(0);
  files = Object.keys(await layer(page)).sort();
  expect(files).toEqual(["content/items.json"]);
  expect((await requests(page)).some((r) => r.cmd === "studio_layer_delete" && r.args.path === "content/twigs.json")).toBe(true);
});

test("without the game-data catalog: the pickers list the pack's mods' items only; fields are typed by hand", async ({ page }) => {
  await goData(page, "&studio=nocatalog");
  await expect(page.getByTestId("data-nocatalog")).toBeVisible({ timeout: 45_000 });
  await page.getByTestId("data-items-add").click();
  await page.getByTestId("data-items-1-sel-input").fill("a");
  await expect(page.getByTestId("data-items-1-sel-hit-Cinderbrand_Sword")).toBeVisible();
  await expect(page.getByTestId("data-items-1-sel-hit-Material_T1_Stone")).toHaveCount(0);
  await shot(page, "data-nocatalog");
});
