// Synthetic saves only. The workflow below uses real Tab/arrow/activation keys, never locator.focus(), clicks or drags.
import { expect, test, type Locator, type Page } from "@playwright/test";

async function open(page: Page) {
  await page.goto("/#/editor");
  await expect(page.locator(".ke .k-page-title")).toContainText("Aldric", { timeout: 15000 });
}
async function tabTo(page: Page, target: Locator, backwards = false) {
  await expect(target).toBeVisible();
  for (let i = 0; i < 360; i++) {
    if (await target.evaluate((el) => el === document.activeElement)) return;
    await page.keyboard.press(backwards ? "Shift+Tab" : "Tab");
  }
  await expect(target).toBeFocused();
}
const slot = (page: Page, key: string) => page.locator(`.ke-grid [data-deck-id="slot-${key}"]`);
async function apprenticeWithKeys(page: Page) {
  const tree = page.getByRole("group", { name: "Skill tree", exact: true });
  const apprentice = tree.getByRole("button", { name: /^Mage Apprentice/ });
  await tabTo(page, tree.getByRole("button").first());
  // Giant Slayer Hook -> Spark -> Spirit -> Mage Apprentice, using the rendered spatial navigation.
  for (let i = 0; i < 8 && !(await apprentice.evaluate((el) => el === document.activeElement)); i++) await page.keyboard.press("ArrowRight");
  await expect(apprentice).toBeFocused();
  return apprentice;
}

test("desktop skill nodes have names, visible focus and keyboard level controls", async ({ page }) => {
  await open(page);
  await tabTo(page, page.getByRole("tab", { name: "Backpack", exact: true }));
  await page.keyboard.press("ArrowRight");
  await expect(page.getByRole("tab", { name: "Skills", exact: true })).toHaveAttribute("aria-selected", "true");
  const tree = page.getByRole("group", { name: "Skill tree", exact: true });
  await expect(tree).toBeVisible();
  const node = await apprenticeWithKeys(page);
  await expect(page.locator(".ke-tree-tip")).toContainText("Mage Apprentice");
  const ring = node.locator("[data-skill-focus]");
  await expect(ring).toBeVisible();
  expect(await ring.evaluate((el) => parseFloat(getComputedStyle(el).strokeWidth))).toBeGreaterThan(0);
  const box = await node.boundingBox(), frame = await tree.boundingBox();
  expect(box && frame && box.x >= frame.x && box.y >= frame.y && box.x + box.width <= frame.x + frame.width && box.y + box.height <= frame.y + frame.height).toBe(true);
  await page.keyboard.press("Enter");
  await expect(node).toHaveAccessibleName(/Mage Apprentice.*3.*3/);
  await page.keyboard.press("-");
  await expect(node).toHaveAccessibleName(/Mage Apprentice.*2.*3/);
  await page.keyboard.press("Space");
  await expect(node).toHaveAccessibleName(/Mage Apprentice.*3.*3/);
  await expect(node).toBeFocused();
});

test("inventory exposes moving, swapping, cancelling and removal without dragging", async ({ page }) => {
  await open(page);
  await tabTo(page, slot(page, "main:0"));
  const move = page.getByRole("button", { name: "Move", exact: true });
  await expect(move).toBeEnabled();
  await tabTo(page, move, true);
  await page.keyboard.press("Enter");
  await expect(slot(page, "main:0")).toBeFocused();
  await expect(page.getByRole("status").filter({ hasText: /Moving/ })).toContainText("Backpack slot 1");
  await page.keyboard.press("ArrowRight");
  await expect(slot(page, "main:1")).toBeFocused();
  await page.keyboard.press("Enter");
  await expect(slot(page, "main:1")).toHaveAccessibleName(/Diamond.*120/);
  await expect(slot(page, "main:0")).toHaveAccessibleName(/Metal Scraps.*250/);
  await expect(slot(page, "main:1")).toBeFocused();
  // M starts another move. Escape restores the source and leaves both items alone.
  await page.keyboard.press("m");
  await page.keyboard.press("ArrowDown");
  await page.keyboard.press("Escape");
  await expect(slot(page, "main:1")).toBeFocused();
  await expect(slot(page, "main:1")).toHaveAccessibleName(/Diamond.*120/);
  await page.keyboard.press("m");
  await page.keyboard.press("ArrowDown");
  await page.keyboard.press("ArrowRight");
  await expect(slot(page, "main:10")).toBeFocused();
  await page.keyboard.press("Space");
  await expect(slot(page, "main:10")).toHaveAccessibleName(/Diamond.*120/);
  await expect(slot(page, "main:1")).not.toHaveClass(/full/);
  await page.keyboard.press("Enter");
  await tabTo(page, page.getByRole("button", { name: "Remove", exact: true }));
  await page.keyboard.press("Enter");
  await expect(slot(page, "main:10")).toBeFocused();
  await expect(slot(page, "main:10")).not.toHaveClass(/full/);
});

test("catalog options and radio filters expose selection and arrow-key navigation", async ({ page }) => {
  await open(page);
  await tabTo(page, slot(page, "main:10"));
  await page.keyboard.press("i");
  const cat = page.getByRole("dialog", { name: "Add item" });
  const list = cat.getByRole("listbox", { name: "Items", exact: true });
  await expect(list.getByRole("option").first()).toBeVisible();
  for (const [label, count] of [["Rarity", 6], ["Tier", 8]] as const) {
    const group = cat.getByRole("radiogroup", { name: label, exact: true });
    await expect(group.getByRole("radio")).toHaveCount(count);
    const all = group.getByRole("radio").first();
    await tabTo(page, all);
    await expect(all).toHaveAttribute("aria-checked", "true");
    await page.keyboard.press("ArrowRight");
    const next = group.getByRole("radio").nth(1);
    await expect(next).toBeFocused();
    await expect(next).toHaveAttribute("aria-checked", "true");
    await expect(all).toHaveAttribute("aria-checked", "false");
    await page.keyboard.press("ArrowLeft");
    await expect(all).toBeFocused();
    await page.keyboard.press("ArrowLeft");
    await expect(group.getByRole("radio").last()).toBeFocused();
    await page.keyboard.press("Home");
    await expect(all).toBeFocused();
  }
  await tabTo(page, cat.getByRole("textbox", { name: "Search", exact: true }));
  await page.keyboard.type("iron");
  await expect(list.getByRole("option", { name: "Iron Bars", exact: true })).toBeVisible();
  await expect.poll(() => list.getByRole("option").count()).toBeGreaterThan(1);
  await tabTo(page, list.getByRole("option").first());
  await expect(list.getByRole("option").first()).toHaveAttribute("aria-selected", "true");
  await page.keyboard.press("ArrowRight");
  await expect(list.getByRole("option").nth(1)).toBeFocused();
  await expect(list.getByRole("option").nth(1)).toHaveAttribute("aria-selected", "true");
  await expect(list.getByRole("option").first()).toHaveAttribute("aria-selected", "false");
  await page.keyboard.press("End");
  await expect(list.getByRole("option").last()).toBeFocused();
  await page.keyboard.press("Escape");
  await expect(slot(page, "main:10")).toBeFocused();
});

test("catalog radiogroups contain checked and unchecked radios", async ({ page }) => {
  await open(page);
  await tabTo(page, slot(page, "main:10"));
  await page.keyboard.press("i");
  for (const [name, count] of [["Rarity", 6], ["Tier", 8]] as const) {
    const radios = page.getByRole("radiogroup", { name, exact: true }).getByRole("radio");
    await expect.soft(radios).toHaveCount(count);
    await expect.soft(page.getByRole("radiogroup", { name, exact: true }).getByRole("radio", { checked: true })).toHaveCount(1);
  }
});

test("catalog destinations use the visible container and slot number", async ({ page }) => {
  await open(page);
  for (const [key, place] of [["extra:2", "Backpack slot 27"], ["main:17", "Swap bar slot 2"], ["main:27", "Action bar slot 4"]]) {
    // This regression isolates destination wording; the complete keyboard-only flow is below.
    await slot(page, key).click();
    await page.keyboard.press("i");
    const cat = page.getByRole("dialog", { name: "Add item" });
    await cat.getByRole("textbox", { name: "Search", exact: true }).fill("iron bar");
    await cat.locator(".ke-cat-grid .ke-slot").first().click();
    await expect(cat.locator(".ke-cat-pick")).toContainText(`Target: ${place}`);
    await page.keyboard.press("Escape");
  }
});

test("keyboard-only: inspect, add, move, remove and adjust a skill, then round-trip the save", async ({ page }) => {
  await open(page);
  await tabTo(page, slot(page, "main:0"));
  await page.keyboard.press("Enter");
  await expect(page.locator(".ke-insp")).toContainText("Diamond");
  await expect(page.locator(".ke-insp").getByRole("spinbutton", { name: "Amount", exact: true })).toHaveValue("120");
  await tabTo(page, slot(page, "main:10"));
  await page.keyboard.press("Enter");
  const cat = page.getByRole("dialog", { name: "Add item" });
  await tabTo(page, cat.getByRole("textbox", { name: "Search", exact: true }));
  await page.keyboard.type("iron bar");
  const iron = cat.getByRole("option", { name: "Iron Bars", exact: true });
  await tabTo(page, iron);
  await page.keyboard.press("Enter");
  await tabTo(page, cat.getByRole("button", { name: "Add", exact: true }));
  await page.keyboard.press("Enter");
  await expect(cat).toHaveCount(0);
  await expect(slot(page, "main:10")).toBeFocused();
  await expect(slot(page, "main:10")).toHaveAccessibleName(/Iron Bars/);
  await page.keyboard.press("m");
  await page.keyboard.press("ArrowRight");
  await page.keyboard.press("Enter");
  await expect(slot(page, "main:11")).toBeFocused();
  await expect(slot(page, "main:10")).not.toHaveClass(/full/);
  await expect(slot(page, "main:11")).toHaveAccessibleName(/Iron Bars/);
  await page.keyboard.press("Delete");
  await expect(slot(page, "main:11")).toBeFocused();
  await expect(slot(page, "main:11")).not.toHaveClass(/full/);
  await tabTo(page, page.getByRole("tab", { name: "Backpack", exact: true }), true);
  await page.keyboard.press("ArrowRight");
  const apprentice = await apprenticeWithKeys(page);
  await page.keyboard.press("Enter");
  await expect(apprentice).toHaveAccessibleName(/Mage Apprentice.*3.*3/);
  await page.keyboard.press("Control+s");
  await expect(page.locator(".ke .k-page-sub")).not.toContainText("Changes not saved");
  await expect(apprentice).toHaveAccessibleName(/Mage Apprentice.*3.*3/);
  await tabTo(page, page.getByRole("button", { name: "Aldric", exact: true }));
  await page.keyboard.press("Enter");
  await tabTo(page, page.getByRole("option", { name: /^Aldric/ }));
  await page.keyboard.press("Enter");
  await expect(apprentice).toHaveAccessibleName(/Mage Apprentice.*3.*3/);
  await tabTo(page, page.getByRole("tab", { name: "Skills", exact: true }), true);
  await page.keyboard.press("ArrowLeft");
  await expect(slot(page, "main:10")).not.toHaveClass(/full/);
  await expect(slot(page, "main:11")).not.toHaveClass(/full/);
  await expect(slot(page, "main:0")).toHaveAccessibleName(/Diamond.*120/);
});
