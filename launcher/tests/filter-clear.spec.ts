// The filter boxes' clear (X) button and Esc (ui/Form.tsx WellInput onClear; promised on Nexus for 0.2.0-dev.29):
// the button shows only while the box has text, a click or Esc empties the box and leaves the focus in it, the button
// is reached by Tab and by the Deck's focus moves (deck/focus.ts), and Esc in an empty box does what it did before (a
// dialog closes). Browser mock (src/api/mock.ts): the main list, Logs, a server's Mods, console and Commands dialog,
// the catalog's Search and the admin's item search.   bun x playwright test tests/filter-clear.spec.ts
import { expect, test, type Locator, type Page } from "@playwright/test";

async function open(page: Page, route = "/", query = "") {
  await page.goto(`/${query}#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
  if (await page.evaluate((r) => location.hash !== `#${r}`, route)) await page.evaluate((r) => { location.hash = `#${r}`; }, route);
}
const rows = (page: Page) => page.locator(".k-hero").getByRole("list", { name: "Mods" }).getByRole("listitem");
const row = (page: Page, name: string) => rows(page).filter({ has: page.getByText(name, { exact: true }) });

/** The box empties by the button and by Esc, keeping the focus; the button is there only while it has text. */
async function clears(box: Locator, button: Locator, text = "zz") {
  await expect(button).toHaveCount(0);
  await box.fill(text);
  await expect(button).toBeVisible();
  await button.click();
  await expect(box).toHaveValue("");
  await expect(box).toBeFocused();
  await expect(button).toHaveCount(0);
  await box.fill(text);
  await box.press("Escape");
  await expect(box).toHaveValue("");
  await expect(box).toBeFocused();
}

test("the main list: the clear button and Esc empty the filter, the focus stays in it, and every row comes back", async ({ page }) => {
  await open(page);
  await expect(row(page, "New Items")).toBeVisible({ timeout: 10_000 });
  const all = await rows(page).count();
  const box = page.locator(".k-hero").getByRole("textbox", { name: "Filter", exact: true });
  const x = page.locator(".k-hero").getByRole("button", { name: "Clear filter" });
  await expect(x).toHaveCount(0);
  await box.fill("chest");
  await expect(row(page, "New Items")).toHaveCount(0);
  await expect(x).toBeVisible();
  await expect(x).toHaveAttribute("title", "Clear filter (Esc)");
  await x.click();
  await expect(box).toHaveValue("");
  await expect(box).toBeFocused();
  await expect(rows(page)).toHaveCount(all);
  await expect(x).toHaveCount(0);
  // Esc, typed in the box
  await box.pressSequentially("chest");
  await expect(row(page, "New Items")).toHaveCount(0);
  await page.keyboard.press("Escape");
  await expect(box).toHaveValue("");
  await expect(box).toBeFocused();
  await expect(rows(page)).toHaveCount(all);
  // the keyboard reaches the button: Tab from the box, Enter clears and gives the focus back
  await box.fill("chest");
  await page.keyboard.press("Tab");
  await expect(x).toBeFocused();
  await page.keyboard.press("Enter");
  await expect(box).toHaveValue("");
  await expect(box).toBeFocused();
  // the Deck UI's focus moves (deck/focus.ts) take it as a stop: right from the box lands on it
  await box.fill("chest");
  await box.focus();
  const moved = await page.evaluate(async () => {
    const f = (await import("/src/deck/focus.ts")) as unknown as { move: (scope: Element, dir: string) => boolean };
    const bar = document.querySelector(".k-hero .k-mods-bar")!;
    return f.move(bar, "right") && document.activeElement?.getAttribute("aria-label");
  });
  expect(moved).toBe("Clear filter");
});

test("Esc in a dialog's filter empties it first; in the empty box Esc closes the dialog as before", async ({ page }) => {
  await open(page, "/servers/lab/overview");
  await page.getByRole("button", { name: "More for the console" }).click();
  await page.getByRole("menuitem", { name: "Kiln commands" }).click();
  const dlg = page.getByRole("dialog", { name: "Commands" });
  const box = dlg.getByRole("textbox", { name: "Filter", exact: true });
  await expect(box).toBeFocused();
  await box.pressSequentially("zz");
  await expect(dlg.getByText("No command matches that.")).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(dlg).toBeVisible();
  await expect(box).toHaveValue("");
  await expect(box).toBeFocused();
  await expect(dlg.getByText("No command matches that.")).toHaveCount(0);
  // the button in the dialog too
  await clears(box, dlg.getByRole("button", { name: "Clear filter" }));
  // empty: Esc is the dialog's again
  await page.keyboard.press("Escape");
  await expect(dlg).toHaveCount(0);
});

test("every other filter box: Logs, a server's mods and console, the catalog's search, the admin's item search", async ({ page }) => {
  await open(page, "/logs");
  await clears(page.locator(".k-console-bar").getByRole("textbox", { name: "Filter", exact: true }), page.locator(".k-console-bar").getByRole("button", { name: "Clear filter" }));
  await open(page, "/servers/lab/logs");
  await clears(page.locator(".k-console-bar").getByRole("textbox", { name: "Filter", exact: true }), page.locator(".k-console-bar").getByRole("button", { name: "Clear filter" }));
  await open(page, "/servers/lab/mods");
  await clears(page.getByRole("textbox", { name: "Filter mods", exact: true }), page.getByRole("button", { name: "Clear filter mods" }));
  await open(page, "/servers/lab/overview");
  const bar = page.locator(".k-console-bar");
  await clears(bar.getByRole("textbox", { name: "Filter", exact: true }), bar.getByRole("button", { name: "Clear filter" }));
  await open(page, "/catalog", "?nexus=premium");
  await clears(page.getByRole("textbox", { name: "Search", exact: true }), page.getByRole("button", { name: "Clear search" }), "night");
  await open(page, "/servers/lab/admin/items");
  await clears(page.getByRole("textbox", { name: "Search items", exact: true }), page.getByRole("button", { name: "Clear search items" }), "wood");
});
