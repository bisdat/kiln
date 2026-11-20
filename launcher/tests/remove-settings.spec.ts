// "Remove its settings too" (owner 2026-11-20; docs/kiln/launcher/app.md, Remove): the remove dialog, desktop and Deck,
// has the box unchecked, so a removed mod's settings stay and come back when it is added again; checked, its settings go
// with its last Library version (library_remove's `settings`, kiln_manage::mod_settings). Removing one version of
// several keeps the mod, so no box. Browser mock: `__kilnMock.settingsRemoved()` lists the mods whose settings went.
import { expect, test, type Page } from "@playwright/test";

const HINT = "Its settings in this game and your profiles go too. Kiln keeps a copy in backups.";
const settingsRemoved = (page: Page) => page.evaluate(() => (window as unknown as { __kilnMock: { settingsRemoved: () => string[] } }).__kilnMock.settingsRemoved());
const libraryIds = (page: Page) => page.evaluate(async () => (await (await import("/src/api/commands.ts")).libraryList()).map((e) => e.id));

test.describe("desktop", () => {
  async function desktop(page: Page) {
    await page.goto("/#/");
    await page.locator(".k-splash").waitFor({ state: "detached" });
  }
  const list = (page: Page) => page.locator(".k-hero").getByRole("list", { name: "Mods" });
  const row = (page: Page, name: string) => list(page).getByRole("listitem").filter({ has: page.getByText(name, { exact: true }) });
  async function openRemove(page: Page, name: string, item = "Remove") {
    await row(page, name).getByRole("button", { name: `More for ${name}` }).click({ timeout: 10_000 });
    await page.getByRole("menuitem", { name: item, exact: true }).click();
  }

  test("the box is there, unchecked: Remove keeps the settings", async ({ page }) => {
    await desktop(page);
    await openRemove(page, "Night Sky");
    const dialog = page.getByRole("dialog", { name: "Remove Night Sky?" });
    const box = dialog.getByRole("checkbox", { name: /Remove its settings too/ });
    await expect(box).toBeVisible();
    await expect(box).not.toBeChecked();
    await expect(dialog).toContainText(HINT);
    await dialog.getByRole("button", { name: "Remove", exact: true }).click();
    await expect(dialog).toBeHidden();
    await expect.poll(() => libraryIds(page)).not.toContain("night-sky");
    expect(await settingsRemoved(page)).toEqual([]);
  });

  test("checked, the settings go with the mod; the next dialog starts unchecked again", async ({ page }) => {
    await desktop(page);
    await openRemove(page, "Night Sky");
    let dialog = page.getByRole("dialog", { name: "Remove Night Sky?" });
    await dialog.getByRole("checkbox", { name: /Remove its settings too/ }).check();
    await dialog.getByRole("button", { name: "Remove", exact: true }).click();
    await expect(dialog).toBeHidden();
    await expect.poll(() => settingsRemoved(page)).toEqual(["kiln/night-sky"]);
    await openRemove(page, "New Items");
    dialog = page.getByRole("dialog", { name: "Remove New Items?" });
    await expect(dialog.getByRole("checkbox", { name: /Remove its settings too/ })).not.toBeChecked();
  });

  test("a mod with several versions: its whole Remove has the box and takes the settings after the last version", async ({ page }) => {
    await desktop(page);
    // one version of several: the mod stays, so do its settings, and there is no box
    await openRemove(page, "More Hotbars", "Remove 2.2.0");
    let dialog = page.getByRole("dialog", { name: "Remove More Hotbars 2.2.0?" });
    await expect(dialog).toBeVisible();
    await expect(dialog.getByRole("checkbox")).toHaveCount(0);
    await dialog.getByRole("button", { name: "Cancel" }).click();
    await openRemove(page, "More Hotbars");
    dialog = page.getByRole("dialog", { name: "Remove More Hotbars?" });
    await dialog.getByRole("checkbox", { name: /Remove its settings too/ }).check();
    await dialog.getByRole("button", { name: "Remove", exact: true }).click();
    await expect(dialog).toBeHidden();
    await expect.poll(() => libraryIds(page)).not.toContain("more-hotbars");
    expect(await settingsRemoved(page)).toEqual(["kiln/more-hotbars"]);
  });

  test("with \"Type a mod's name to remove it\" on, the box sits above the typed name", async ({ page }) => {
    await page.addInitScript(() => localStorage.setItem("kiln.confirm.typeNames", "1"));
    await desktop(page);
    await openRemove(page, "Night Sky");
    const dialog = page.getByRole("dialog", { name: "Remove Night Sky?" });
    const go = dialog.getByRole("button", { name: "Remove", exact: true });
    await dialog.getByRole("checkbox", { name: /Remove its settings too/ }).check();
    await expect(go).toBeDisabled();
    await dialog.getByRole("textbox").fill("night-sky");
    await go.click();
    await expect(dialog).toBeHidden();
    await expect.poll(() => settingsRemoved(page)).toEqual(["kiln/night-sky"]);
  });

  test("Remove… of several selected mods: one box for all of them", async ({ page }) => {
    await desktop(page);
    await page.getByRole("combobox", { name: "Kind" }).selectOption({ label: "Kiln" });
    const box = (name: string) => list(page).getByRole("checkbox", { name: `Select ${name}` });
    await box("Big Stacks").click();
    await box("New Items").click({ modifiers: ["Shift"] });
    await page.getByRole("toolbar", { name: "Selected mods" }).getByRole("button", { name: "Remove…" }).click();
    const dialog = page.getByRole("dialog", { name: "Remove 3 mods?" });
    const settings = dialog.getByRole("checkbox", { name: /Remove their settings too/ });
    await expect(settings).not.toBeChecked();
    await expect(dialog).toContainText("Their settings in this game and your profiles go too. Kiln keeps a copy in backups.");
    await settings.check();
    const go = dialog.getByRole("button", { name: "Remove 3" });
    await expect(go).toBeEnabled({ timeout: 10_000 });
    await go.click();
    await expect(dialog).toBeHidden();
    await expect.poll(async () => (await settingsRemoved(page)).sort()).toEqual(["kiln/big-stacks", "kiln/more-hotbars", "kiln/new-items"]);
  });
});

test.describe("Deck", () => {
  test.use({ viewport: { width: 1280, height: 800 } });
  async function pad(page: Page, button: string) {
    const send = (pressed: boolean) => page.evaluate(([b, p]) => (window as unknown as { __kilnMock: { emit: (n: string, v: unknown) => void } }).__kilnMock
      .emit("kiln://gamepad", { kind: "button", button: b, pressed: p, repeat: false, device: "event9" }), [button, pressed] as const);
    await send(true);
    await send(false);
    await page.waitForTimeout(60);
  }
  const focused = (page: Page) => page.evaluate(() => (document.activeElement as HTMLElement | null)?.dataset?.deckId ?? null);
  async function removeDialog(page: Page) {
    await page.goto("/?deck=steamdeck#/");
    await expect(page.locator('[data-deck-screen="home"]')).toBeVisible();
    await expect.poll(() => focused(page)).toBe("mouth");
    await pad(page, "rb");
    await expect.poll(() => focused(page)).toBe("mod-emf-beastmaster");
    await page.locator('[data-deck-id="mod-kiln-new-items"]').focus();
    await pad(page, "x");
    const dialog = page.locator('[data-deck-dialog="remove"]');
    await expect(dialog).toBeVisible();
    await expect(dialog.getByRole("button", { name: "Remove", exact: true })).toBeFocused();
    return dialog;
  }

  test("X, then Remove: the settings stay by default", async ({ page }) => {
    const dialog = await removeDialog(page);
    const settings = dialog.locator('[data-deck-id="remove-settings"]');
    await expect(settings).toContainText("Remove its settings too");
    await expect(settings).toContainText(HINT);
    await expect(settings).toContainText("Off");
    await pad(page, "a");
    await expect(dialog).toHaveCount(0);
    await expect.poll(() => libraryIds(page)).not.toContain("new-items");
    expect(await settingsRemoved(page)).toEqual([]);
  });

  test("A on the settings row turns it on; Remove then takes the settings too", async ({ page }) => {
    const dialog = await removeDialog(page);
    const settings = dialog.locator('[data-deck-id="remove-settings"]');
    await settings.focus();
    await pad(page, "a");
    await expect(settings).toContainText("On");
    await dialog.getByRole("button", { name: "Remove", exact: true }).focus();
    await pad(page, "a");
    await expect(dialog).toHaveCount(0);
    await expect.poll(() => settingsRemoved(page)).toEqual(["kiln/new-items"]);
  });
});
