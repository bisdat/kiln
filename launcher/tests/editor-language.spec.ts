// The save editor (src/editor/kiln, src/editor/deck) in the launcher's languages, in mock mode with the mock's synthetic
// saves (src/api/mockEditor.ts): its words are the launcher's ("editor.*" in src/i18n/*.json), so with the system in
// Russian or Chinese (the launcher's default, System) the page, its tabs, the catalog, the skill tree, knowledge and the
// Deck screen speak that language; no key shows in place of a text, and none of upstream's Portuguese or English shows
// where a translation has the text. `KILN_EDITOR_SHOTS=<dir>` also saves screenshots there.
import { expect, test, type Browser, type Page } from "@playwright/test";
import { mkdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

type Tree = { [k: string]: string | Tree };
const read = (tag: string) => JSON.parse(readFileSync(join(import.meta.dirname, "..", "src", "i18n", `${tag}.json`), "utf8")) as Tree;
const at = (t: Tree, key: string): string => {
  const v = key.split(".").reduce<string | Tree | undefined>((o, k) => (o && typeof o === "object" ? o[k] : undefined), t);
  if (typeof v !== "string") throw new Error(`no text at ${key}`);
  return v;
};
function keysOf(o: Tree, prefix = "", out: string[] = []): string[] {
  for (const [k, v] of Object.entries(o)) {
    const key = prefix ? `${prefix}.${k}` : k;
    if (typeof v === "object") keysOf(v, key, out);
    else out.push(key);
  }
  return out;
}
const EN = read("en");
const KEYS = keysOf(EN).filter((k) => k.startsWith("editor."));
/** Editor keys shown as text (a text neither the language nor English has). */
const keysShown = (page: Page) => page.evaluate((keys) => keys.filter((k) => document.body.innerText.includes(k)), KEYS);
/** Upstream's Portuguese words the old UI could leak (none of them is a word of these languages). */
const PORTUGUESE = ["Mochila", "Habilidades", "Conhecimento", "Salvar", "Raridade", "Quantidade", "Nível", "espaços", "Adicionar"];
const portugueseShown = (page: Page) => page.evaluate((words) => words.filter((w) => document.body.innerText.includes(w)), PORTUGUESE);

async function shot(page: Page, name: string) {
  const dir = process.env.KILN_EDITOR_SHOTS;
  if (!dir) return;
  mkdirSync(dir, { recursive: true });
  await page.waitForTimeout(800);
  await page.screenshot({ path: join(dir, `${name}.png`) });
}

/** A page whose system language is `tag` (the launcher's default language setting is System). */
async function pageIn(browser: Browser, tag: string, viewport?: { width: number; height: number }) {
  const context = await browser.newContext({ locale: tag, ...(viewport ? { viewport } : {}) });
  const page = await context.newPage();
  await page.addInitScript((l) => Object.defineProperty(navigator, "languages", { get: () => [l] }), tag);
  return page;
}

for (const tag of ["ru", "zh-CN", "de"]) {
  test(`desktop editor in ${tag}: page, tabs, catalog, skills and knowledge`, async ({ browser }) => {
    const L = read(tag);
    const page = await pageIn(browser, tag);
    await page.goto("/#/editor");
    await expect(page.locator(".ke .k-page-title")).toContainText("Aldric", { timeout: 15000 });
    await expect.poll(() => page.evaluate(() => document.documentElement.lang)).toBe(tag);
    const tabs = page.getByRole("tab");
    for (const k of ["equipment", "inventory", "skills", "knowledge"]) await expect(tabs.filter({ hasText: at(L, `editor.tabs.${k}`) })).toHaveCount(1);
    await expect(page.locator(".ke .k-page-actions")).toContainText(at(L, "editor.page.save"));
    await expect(page.locator(".ke-group-head").first()).toContainText(at(L, "editor.tabs.inventory"));
    await expect(page.locator(".ke-insp .ke-empty")).toContainText(at(L, "editor.inventory.noneSelected"));
    // an item: the inspector's labels
    await page.locator(".ke-grid .ke-slot.full").first().click();
    await expect(page.locator(".ke-insp")).toContainText(at(L, "editor.common.amount"));
    expect(await keysShown(page)).toEqual([]);
    expect(await portugueseShown(page)).toEqual([]);
    await shot(page, `editor-inventory-${tag}`);
    // the catalog: its title, categories and the count line (a plural)
    await page.locator(".ke-group-head button").first().click();
    const cat = page.getByRole("dialog");
    await expect(cat).toContainText(at(L, "editor.inventory.addItem"));
    await expect(cat.locator(".ke-cat-rail")).toContainText(at(L, "editor.catalog.cat.weapons"));
    expect(await keysShown(page)).toEqual([]);
    await shot(page, `editor-catalog-${tag}`);
    await page.keyboard.press("Escape");
    await expect(cat).toHaveCount(0);
    // equipment: slot names and attributes
    await tabs.filter({ hasText: at(L, "editor.tabs.equipment") }).click();
    await expect(page.locator(".ke-gear")).toContainText(at(L, "editor.equipment.groups.armour"));
    await expect(page.locator(".ke-attrs")).toContainText(at(L, "editor.game.attribute.strength"));
    expect(await keysShown(page)).toEqual([]);
    expect(await portugueseShown(page)).toEqual([]);
    await shot(page, `editor-equipment-${tag}`);
    // the skill tree's points
    await tabs.filter({ hasText: at(L, "editor.tabs.skills") }).click();
    await expect(page.locator(".ke-tree-points")).toContainText(at(L, "editor.skills.used"));
    expect(await keysShown(page)).toEqual([]);
    await shot(page, `editor-skills-${tag}`);
    // knowledge: filters, columns and the count line
    await tabs.filter({ hasText: at(L, "editor.tabs.knowledge") }).click();
    await expect(page.locator(".ke-know-bar")).toContainText(at(L, "editor.knowledge.filter.itemsRecipes"));
    await expect(page.locator(".ke-know-row.head")).toContainText(at(L, "editor.knowledge.state"));
    expect(await keysShown(page)).toEqual([]);
    expect(await portugueseShown(page)).toEqual([]);
    await shot(page, `editor-knowledge-${tag}`);
    // backups
    await page.locator(".ke .k-page-actions button").filter({ hasText: at(L, "editor.page.backups") }).click();
    await expect(page.getByRole("dialog")).toContainText(at(L, "editor.backups.openFolder"));
    expect(await keysShown(page)).toEqual([]);
    await page.context().close();
  });
}

test("the editor follows a change of language while it is open", async ({ browser }) => {
  const page = await pageIn(browser, "en");
  await page.goto("/#/editor");
  await expect(page.locator(".ke .k-page-title")).toContainText("Aldric", { timeout: 15000 });
  await expect(page.getByRole("tab").filter({ hasText: "Backpack" })).toHaveCount(1);
  // Settings, Language, then back to the editor: its session (and the open character) stays, its words change
  await page.evaluate(() => { location.hash = "#/settings"; });
  await page.locator("#st-lang").selectOption("ru");
  await page.evaluate(() => { location.hash = "#/editor"; });
  await expect(page.getByRole("tab").filter({ hasText: at(read("ru"), "editor.tabs.inventory") })).toHaveCount(1);
  expect(await keysShown(page)).toEqual([]);
  await page.context().close();
});

test("Deck editor in ru and zh-CN: the screen, its buttons and tabs", async ({ browser }) => {
  for (const tag of ["ru", "zh-CN"]) {
    const L = read(tag);
    const page = await pageIn(browser, tag, { width: 1280, height: 800 });
    await page.goto("/?deck=steamdeck#/deck/editor");
    await expect(page.locator(".deck-editor .dke-name")).toContainText("Aldric", { timeout: 15000 });
    await expect(page.locator(".dke-buttons")).toContainText(at(L, "editor.page.characters"));
    await expect(page.locator(".dke-buttons")).toContainText(at(L, "editor.page.save"));
    await expect(page.locator(".dke-tabs")).toContainText(at(L, "editor.tabs.knowledge"));
    await expect(page.getByTestId("deck-editor-say")).not.toContainText("Kiln backs");
    expect(await keysShown(page)).toEqual([]);
    expect(await portugueseShown(page)).toEqual([]);
    await shot(page, `deck-editor-${tag}`);
    await page.context().close();
  }
});
