// Item/Knowledge controls against the real editor and the mock's serialized SV2C save files, never player saves.
import { expect, test, type Page } from "@playwright/test";

async function open(page: Page) {
  await page.goto("/#/editor");
  await expect(page.locator(".k-page-title")).toContainText("Aldric", { timeout: 15000 });
}
async function disk(page: Page) {
  return page.evaluate(async () => {
    const sessionPath = "/src/editor/kiln/session.js", platformPath = "/src/editor/src/platform.js";
    const { useSession } = await import(sessionPath);
    const { api } = await import(platformPath);
    // openSave reads editor_read_file and parses the persisted bytes, not the editor's current model.
    return api.openSave(useSession.getState().model.path);
  });
}
async function save(page: Page) {
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await expect(page.locator(".ke .k-page-sub")).not.toContainText("Changes not saved");
}

test("clearing 120 and typing 250 persists 250, not 1250", async ({ page }) => {
  await open(page);
  await page.locator(".ke-grid .ke-slot.full").first().click();
  const amount = page.getByRole("spinbutton", { name: "Amount", exact: true });
  await amount.focus();
  await page.keyboard.press("Control+a");
  await page.keyboard.press("Backspace");
  await page.keyboard.type("250");
  await amount.press("Enter");
  await save(page);
  expect((await disk(page)).main.find((e: { slot: number }) => e.slot === 0).count).toBe(250);
});

test("clearing 120 and typing 250 then Ctrl+S immediately persists the complete draft", async ({ page }) => {
  await open(page);
  await page.locator(".ke-grid .ke-slot.full").first().click();
  const amount = page.getByRole("spinbutton", { name: "Amount", exact: true });
  await amount.fill("");
  await amount.pressSequentially("250");
  await amount.press("Control+s");
  await expect.poll(async () => (await disk(page)).main.find((e: { slot: number }) => e.slot === 0).count).toBe(250);
});

test("numeric drafts normalize on blur, Enter and step through the existing byte limits", async ({ page }) => {
  await open(page);
  await page.locator(".ke-grid .ke-slot.full").first().click();
  const amount = page.getByRole("spinbutton", { name: "Amount", exact: true });
  await amount.fill("");
  await expect(amount).toHaveValue("");
  await expect(page.locator(".ke-grid .ke-slot.full").first()).toContainText("120");
  await amount.fill("249");
  await amount.press("Tab");
  await expect(amount).toHaveValue("249");
  await amount.fill("65536");
  await page.locator('.ke-insp [aria-label="−1"]').first().click();
  await expect(amount).toHaveValue("65534"); // steps start at the byte-clamped committed value
  await amount.fill("65536");
  await amount.press("Enter");
  await expect(amount).toHaveValue("65535");
  await amount.fill("250");
  await page.locator('.ke-insp [aria-label="+1"]').first().click();
  await expect(amount).toHaveValue("251");
  await amount.fill("-10");
  await amount.press("Tab");
  await expect(amount).toHaveValue("1");
  await amount.fill("");
  await amount.pressSequentially("1e");
  await amount.press("Enter");
  await expect(amount).toHaveValue("1");
  await amount.fill("250.9");
  await amount.press("Enter");
  await expect(amount).toHaveValue("250");
  await save(page);
  expect((await disk(page)).main.find((e: { slot: number }) => e.slot === 0).count).toBe(250);
});

for (const control of [
  { label: "Amount", slot: 0, key: "count", min: 1, max: 65535 },
  { label: "Level", slot: 24, key: "level", min: 2, max: 255 },
  { label: "Gem level", slot: 24, key: "gemLevel", min: 1, max: 255 },
]) {
  test(`${control.label} keyboard steps use the byte-clamped draft and never publish out-of-range values`, async ({ page }) => {
    await open(page);
    await page.locator(`[data-deck-id="slot-main:${control.slot}"]`).click();
    await page.evaluate(async ({ slot, key }) => {
      const path = "/src/editor/kiln/session.js";
      const { useSession } = await import(path);
      (window as any).__controlValues = [];
      useSession.subscribe((s: any) => {
        const entry = s.model.main.find((e: any) => e.slot === slot);
        (window as any).__controlValues.push(key === "gemLevel" ? parseInt(entry.socket.slice(8), 16) : entry[key]);
      });
    }, control);
    const input = page.getByRole("spinbutton", { name: control.label, exact: true });
    // Like the minus button, ArrowDown must step from the committed maximum, not from the unclamped text.
    await input.fill(String(control.max + 1));
    await input.press("ArrowDown");
    await expect(input).toHaveValue(String(control.max - 1));
    await input.press("Control+s");
    await expect.poll(async () => {
      const entry = (await disk(page)).main.find((e: { slot: number }) => e.slot === control.slot);
      return control.key === "gemLevel" ? parseInt(entry.socket.slice(8), 16) : entry[control.key];
    }).toBe(control.max - 1);
    // The same rule applies at the lower boundary; keyboard and pointer steps must agree.
    await input.fill(String(control.min - 1));
    await input.press("ArrowUp");
    await expect(input).toHaveValue(String(control.min + 1));
    await input.press("Control+s");
    await expect.poll(async () => {
      const entry = (await disk(page)).main.find((e: { slot: number }) => e.slot === control.slot);
      return control.key === "gemLevel" ? parseInt(entry.socket.slice(8), 16) : entry[control.key];
    }).toBe(control.min + 1);
    const values: number[] = await page.evaluate(() => (window as any).__controlValues);
    expect(values.length).toBeGreaterThan(0);
    expect(values.every((v) => Number.isInteger(v) && v >= control.min && v <= control.max)).toBe(true);
  });
}

for (const filter of ["search", "level", "kind"]) {
  test(`Knowledge selection hidden by ${filter} cannot delete persisted entries`, async ({ page }) => {
    await open(page);
    await page.getByRole("tab", { name: /^Knowledge/ }).click();
    const box = page.locator(".ke-know-body input[type=checkbox]").first();
    const id = Number((await box.getAttribute("data-deck-id"))!.slice(5));
    await box.check();
    if (filter === "search") await page.getByRole("textbox", { name: "Search by name or id" }).fill("no-match-123456");
    if (filter === "level") await page.locator('.ke-know-above input, .ke-know-bar input[type=number]').fill("999");
    if (filter === "kind") {
      const kind = await box.locator("xpath=ancestor::label").locator(".ke-kind").textContent();
      await page.getByRole("radio", { name: kind === "item" ? "Recipes only" : "Items only", exact: true }).click();
    }
    const remove = page.locator(".ke-know-bar .k-btn-danger");
    // On the old code the hidden selection could be removed and persisted.
    if (await remove.isEnabled()) await remove.click();
    // Make an unrelated valid edit so even a cleared selection goes through a serialized Save round trip.
    await page.getByRole("tab", { name: "Backpack", exact: true }).click();
    await page.locator(".ke-grid .ke-slot.full").first().click();
    await page.locator('.ke-insp [aria-label="+1"]').first().click();
    await save(page);
    const persisted = await disk(page);
    expect(persisted.knowledge.some((k: { id: number }) => k.id === id)).toBe(true);
    expect(persisted.knowledge).toHaveLength(2000);
  });
}

test("bulk Knowledge removal asks first; cancel preserves entries and confirm round-trips", async ({ page }) => {
  await open(page);
  await page.getByRole("tab", { name: /^Knowledge/ }).click();
  const boxes = page.locator(".ke-know-body input[type=checkbox]");
  const ids = await boxes.evaluateAll((els) => els.slice(0, 2).map((el) => Number((el as HTMLElement).dataset.deckId!.slice(5))));
  await boxes.nth(0).check();
  await boxes.nth(1).check();
  await page.getByRole("button", { name: "Remove 2", exact: true }).click();
  const confirm = page.getByRole("dialog", { name: "Remove 2 knowledge entries?" });
  await expect(confirm).toBeVisible();
  await expect(confirm).toContainText("Recipes that depend on these entries may also close.");
  expect((await disk(page)).knowledge).toHaveLength(2000);
  await confirm.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(page.locator(".ke-know-row.on")).toHaveCount(2);
  await page.getByRole("button", { name: "Remove 2", exact: true }).click();
  await confirm.getByRole("button", { name: "Remove 2", exact: true }).click();
  await expect(confirm).toHaveCount(0);
  await save(page);
  const persisted = await disk(page);
  expect(persisted.knowledge).toHaveLength(1998);
  expect(persisted.knowledge.filter((k: { id: number }) => ids.includes(k.id))).toEqual([]);
});

test("Deck bulk Knowledge confirmation supports controller cancel and confirm", async ({ page }) => {
  await page.goto("/?deck=steamdeck#/deck/editor");
  await expect(page.locator(".dke-name")).toHaveText("Aldric", { timeout: 15000 });
  await page.locator('[data-deck-id="ed-tab-know"]').click();
  const boxes = page.locator(".ke-know-body input[type=checkbox]");
  await boxes.nth(0).check();
  await boxes.nth(1).check();
  const pad = async (button: string) => {
    await page.evaluate((b) => {
      const mock = (window as any).__kilnMock;
      for (const pressed of [true, false]) mock.emit("kiln://gamepad", { kind: "button", button: b, pressed, repeat: false, device: "event9" });
    }, button);
  };
  const focused = () => page.evaluate(() => (document.activeElement as HTMLElement)?.dataset.deckId);
  await page.locator('[data-deck-id="know-remove"]').click();
  const confirm = page.getByRole("dialog", { name: "Remove 2 knowledge entries?" });
  await expect(confirm).toBeVisible();
  await expect.poll(focused).toBe("know-cancel");
  await pad("a");
  await expect(confirm).toHaveCount(0);
  await expect(page.locator(".ke-know-row.on")).toHaveCount(2);
  await page.locator('[data-deck-id="know-remove"]').click();
  await expect.poll(focused).toBe("know-cancel");
  await pad("right");
  await expect.poll(focused).toBe("know-confirm");
  await pad("a");
  await expect(confirm).toHaveCount(0);
  await expect(page.locator('[data-deck-id="ed-tab-know"] .dke-count')).toHaveText("1998");
  await pad("x");
  await expect(page.locator(".deck-toast")).toContainText("Saved");
  expect((await disk(page)).knowledge).toHaveLength(1998);
});

test("a gem overlay merged after the editor loads appears in Catalog and socket choices", async ({ page }) => {
  await open(page);
  await page.evaluate(async () => {
    const path = "/src/editor/src/gamedata.js";
    const { mergeModded, items } = await import(path);
    const base = Object.values(items).find((it: any) => it.category === "WeaponGem") as any;
    mergeModded({ items: [{ id: 0x7f001234, base: base.debug, debug: "Test_Gem", names: { en: "Overlay test gem" }, modId: "test-gem" }] });
  });
  await page.getByRole("button", { name: "Add item", exact: true }).click();
  const catalog = page.getByRole("dialog");
  await catalog.getByRole("textbox").fill("Overlay test gem");
  await expect(catalog.getByRole("button", { name: "Overlay test gem", exact: true })).toBeVisible();
  await page.keyboard.press("Escape");
  await page.getByRole("button", { name: "Eclipsed Blade", exact: true }).click();
  const gem = page.getByRole("combobox", { name: "Socketed gem", exact: true });
  await expect(gem.locator('option[value="2130711092"]')).toHaveText("Overlay test gem");
  await gem.selectOption(String(0x7f001234));
  await save(page);
  expect((await disk(page)).main.find((e: { slot: number }) => e.slot === 24).socket.slice(0, 8)).toBe("3412007f");
});

test("durability and water meters support arrows, Page keys, Home and End and persist", async ({ page }) => {
  await open(page);
  await page.getByRole("button", { name: "Eclipsed Blade", exact: true }).click();
  const durability = page.getByRole("slider", { name: "Durability", exact: true });
  await expect(durability).toHaveAttribute("aria-valuemax", "300");
  await durability.focus();
  await durability.press("Home");
  await expect(durability).toHaveAttribute("aria-valuenow", "0");
  await durability.press("ArrowRight");
  await durability.press("ArrowUp");
  await expect(durability).toHaveAttribute("aria-valuenow", "2");
  await durability.press("PageUp");
  await expect(durability).toHaveAttribute("aria-valuenow", "32");
  await durability.press("PageDown");
  await durability.press("End");
  await durability.press("ArrowRight");
  await expect(durability).toHaveAttribute("aria-valuenow", "300");
  await durability.press("ArrowDown");
  await page.locator('[data-deck-id="slot-main:4"]').click();
  const water = page.getByRole("slider", { name: "Water", exact: true });
  await water.focus();
  await water.press("Home");
  await water.press("ArrowLeft");
  await expect(water).toHaveAttribute("aria-valuenow", "0");
  await water.press("End");
  await water.press("ArrowLeft");
  const liquid = Number(await water.getAttribute("aria-valuenow"));
  await save(page);
  const persisted = await disk(page);
  expect(persisted.main.find((e: { slot: number }) => e.slot === 24).durability).toBe(299);
  expect(persisted.main.find((e: { liquid?: number }) => e.liquid !== undefined).liquid).toBe(liquid);
});

test("read-only Knowledge controls are disabled while item and Knowledge browsing still work", async ({ page }) => {
  await open(page);
  await page.locator(".ke-who-btn").click();
  await page.getByRole("option", { name: /Mordred/ }).click();
  await expect(page.locator(".k-page-title")).toContainText("Mordred");
  await page.locator(".ke-grid .ke-slot.full").first().click();
  await expect(page.locator(".ke-insp")).toContainText("Diamond");
  await page.getByRole("button", { name: "Eclipsed Blade", exact: true }).click();
  await expect(page.locator(".ke-insp")).toContainText("Eclipsed Blade");
  // Inspector's disabled prop is supplied by Inventory/Equipment (tested directly in editor-controls.test.ts).
  // Knowledge owns its read-only state and keeps filters available for browsing.
  await page.getByRole("tab", { name: /^Knowledge/ }).click();
  await expect(page.getByRole("textbox", { name: "Search by name or id" })).toBeEnabled();
  await page.getByRole("radio", { name: "Everything", exact: true }).click();
  await expect(page.locator(".ke-know-body input[type=checkbox]").first()).toBeDisabled();
  await expect(page.getByRole("checkbox", { name: "All", exact: true })).toBeDisabled();
  await page.getByRole("textbox", { name: "Search by name or id" }).fill("no-match-123456");
  await expect(page.locator(".ke-know-body .ke-know-row")).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Save", exact: true })).toBeDisabled();
});
