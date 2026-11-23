// Roll back (dev.29, browser mock with `?kept=1`: Zoom Out's earlier 0.1.0 is kept, with a settings file): the mod's
// menu offers "Roll back to 0.1.0" instead of "Use 0.1.0", the roll back is a profile change with Undo, and the
// version rolled away from is kept; Settings > Library lists kept versions with their size, each one removable.
import { expect, test, type Page } from "@playwright/test";

async function open(page: Page, route = "/") {
  await page.goto(`/?kept=1#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}
const api = <T,>(page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(async ([f, a]) => {
    const m = (await import("/src/api/commands.ts")) as unknown as Record<string, (...x: unknown[]) => Promise<unknown>>;
    return m[f as string](...(a as unknown[]));
  }, [fn, args] as const) as Promise<T>;
type Mods = Record<string, { id: string; version: string; hash: string; enabled: boolean }[]>;
const zoomOut = async (page: Page) => (await api<{ mods: Mods }>(page, "profileGet", "ember-run")).mods.greeneye.find((m) => m.id === "zoom-out");
const row = (page: Page) => page.locator(".k-hero").getByRole("list", { name: "Mods" }).getByRole("listitem")
  .filter({ has: page.getByText("Zoom Out", { exact: true }), hasNot: page.locator(".k-mod-where") }); // not the game folder's row
const menu = async (page: Page) => {
  await row(page).getByRole("button", { name: "More for Zoom Out" }).click();
  return page.getByRole("menu");
};

test("Roll back to the kept version, then Undo, then roll back twice", async ({ page }) => {
  await open(page);
  await expect(row(page)).toBeVisible({ timeout: 10_000 });
  let m = await menu(page);
  await expect(m.getByRole("menuitem", { name: "Roll back to 0.1.0" })).toBeVisible();
  await expect(m.getByRole("menuitem", { name: "Use 0.1.0" })).toHaveCount(0);
  await m.getByRole("menuitem", { name: "Roll back to 0.1.0" }).click();
  const done = page.locator(".k-toast", { hasText: "Zoom Out is back on 0.1.0" });
  await expect(done).toBeVisible();
  await expect(done).toContainText("Its settings from before the update come back with it (1 file). 0.2.0 is kept");
  expect((await zoomOut(page))?.version).toBe("0.1.0");
  await expect(row(page).locator(".k-mod-ver")).toHaveText("0.1.0");
  // Undo: the profile change goes back
  await done.getByRole("button", { name: "Undo" }).click();
  await expect(page.locator(".k-toast", { hasText: "Zoom Out is on 0.2.0 again" })).toBeVisible();
  expect((await zoomOut(page))?.version).toBe("0.2.0");
  // and 0.1.0 is kept again: roll back, then roll back once more returns to 0.2.0
  m = await menu(page);
  await m.getByRole("menuitem", { name: "Roll back to 0.1.0" }).click();
  await expect(page.locator(".k-toast", { hasText: "Zoom Out is back on 0.1.0" }).last()).toBeVisible();
  m = await menu(page);
  await m.getByRole("menuitem", { name: "Roll back to 0.2.0" }).click();
  await expect(page.locator(".k-toast", { hasText: "Zoom Out is back on 0.2.0" })).toBeVisible();
  expect((await zoomOut(page))?.version).toBe("0.2.0");
});

test("Settings > Library lists the kept version with its size, and Remove takes it out of the Library", async ({ page }) => {
  await open(page, "/settings/mods");
  const card = page.getByRole("region", { name: "Library" });
  await expect(card).toContainText("Kept for a roll back");
  // the list opens on Show
  await expect(card.getByRole("list", { name: "Kept versions" })).toHaveCount(0);
  await card.getByRole("button", { name: "Show" }).click();
  const kept = card.getByRole("list", { name: "Kept versions" }).getByRole("listitem");
  await expect(kept).toHaveCount(1);
  await expect(kept).toContainText("Zoom Out 0.1.0");
  await expect(kept).toContainText("1 settings file, replaced by 0.2.0");
  await kept.getByRole("button", { name: "Remove Zoom Out 0.1.0" }).click();
  const dialog = page.getByRole("dialog");
  await expect(dialog).toContainText("you can no longer roll back to it");
  await dialog.getByRole("button", { name: "Remove" }).click();
  await expect(page.locator(".k-toast", { hasText: "Removed Zoom Out 0.1.0" })).toBeVisible();
  await expect(card.getByRole("list", { name: "Kept versions" })).toHaveCount(0);
  // Clean up never offered it
  const lib = await api<{ id: string; versions: { version: string }[] }[]>(page, "libraryList", null);
  expect(lib.find((e) => e.id === "zoom-out")?.versions.map((v) => v.version)).toEqual(["0.2.0"]);
});

test("Settings keeps the count of earlier versions", async ({ page }) => {
  await open(page, "/settings/mods");
  const input = page.locator("#st-keep-versions");
  await expect(input).toHaveValue("1");
  await input.fill("3");
  await input.blur();
  await expect.poll(async () => (await api<{ keepVersions?: number }>(page, "appSettingsGet")).keepVersions).toBe(3);
});
