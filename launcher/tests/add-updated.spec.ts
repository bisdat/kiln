// Inbox 90 (29.18): adding a newer file of a mod the profile already has said "Added {name}". It says "Updated {name}:
// {old} → {new}" now, on a drop (desktop) and on the Deck's Add mods; a new mod still says "Added", the same version
// again what it said before ("is on already", or "Added" for one that was off), and a drop of both kinds names both.
import { expect, test, type Page } from "@playwright/test";

async function desktop(page: Page) {
  await page.goto("/#/");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.locator(".k-door")).toHaveAccessibleName("Play Ember run", { timeout: 10_000 });
}

/** Drops files (by name: mock mode) on Play, as the browser does. */
async function drop(page: Page, ...files: string[]) {
  const dt = await page.evaluateHandle((names) => { const d = new DataTransfer(); for (const n of names) d.items.add(new File(["x"], n)); return d; }, files);
  await page.locator(".k-hero").dispatchEvent("dragover", { dataTransfer: dt });
  await page.locator(".k-hero").dispatchEvent("drop", { dataTransfer: dt });
}

const toastWith = (page: Page, text: string) => page.locator(".k-toast").filter({ has: page.locator(".k-toast-title", { hasText: text }) });

test("a newer file of a mod in the profile says Updated with both versions; Undo puts the old one back", async ({ page }) => {
  await desktop(page);
  // Ember run has More Hotbars 2.1.0
  await drop(page, "more-hotbars-2.2.0.zip");
  const t = toastWith(page, "Updated More Hotbars: 2.1.0 → 2.2.0");
  await expect(t).toBeVisible({ timeout: 8000 });
  await expect(page.locator(".k-toast").filter({ hasText: "Added More Hotbars" })).toHaveCount(0);
  await t.getByRole("button", { name: "Undo" }).click();
  await expect(page.locator(".k-toast").filter({ hasText: "Undone" })).toBeVisible({ timeout: 5000 });
});

test("a new mod says Added; the same version again says what it said before", async ({ page }) => {
  await desktop(page);
  await drop(page, "night-owl-1.2.0.zip");
  await expect(toastWith(page, "Added Night Owl")).toBeVisible({ timeout: 8000 });
  // the version the profile has, on: already on, as before
  await drop(page, "more-hotbars-2.1.0.zip");
  await expect(page.locator(".k-toast").filter({ hasText: "More Hotbars is on already" })).toBeVisible({ timeout: 8000 });
  // the version the profile has, switched off there (Big Stacks 0.3.1): switched on, "Added" as before
  await drop(page, "big-stacks-0.3.1.zip");
  await expect(toastWith(page, "Added Big Stacks")).toBeVisible({ timeout: 8000 });
});

test("a drop with a new mod and a newer file names both", async ({ page }) => {
  await desktop(page);
  await drop(page, "night-owl-1.2.0.zip", "weather-plus-1.5.0.zip");
  await expect(toastWith(page, "Added Night Owl, updated Weather Plus")).toBeVisible({ timeout: 8000 });
});

test.describe("deck", () => {
  test.use({ viewport: { width: 1280, height: 800 } });

  const pad = async (page: Page, button: string) => {
    for (const pressed of [true, false]) await page.evaluate(([b, p]) => (window as unknown as { __kilnMock: { emit: (n: string, v: unknown) => void } }).__kilnMock
      .emit("kiln://gamepad", { kind: "button", button: b, pressed: p, repeat: false, device: "event9" }), [button, pressed] as const);
    await page.waitForTimeout(60);
  };
  const focused = (page: Page) => page.evaluate(() => (document.activeElement as HTMLElement | null)?.dataset?.deckId ?? null);

  test("Add mods says Updated for a newer file of a mod in the profile", async ({ page }) => {
    await page.goto("/?deck=steamdeck#/");
    await expect(page.locator('[data-deck-screen="home"]')).toBeVisible();
    // Lantern Glow 1.1.0 is in the profile first (added on the desktop, say); Downloads has 1.2.0
    await page.evaluate(async () => {
      const api = await import("/src/api/commands.ts");
      const id = await api.libraryAdd("client-steam", ["lantern-glow-1.1.0.zip"]);
      for (let i = 0; i < 100; i++) { const task = await api.taskGet(id); if (task.state === "done") return; await new Promise((r) => setTimeout(r, 100)); }
      throw new Error("the add did not end");
    });
    await expect.poll(() => focused(page)).toBe("mouth");
    await pad(page, "rb");
    await pad(page, "rb");
    await expect(page.locator(".deck-title")).toHaveText("Downloads");
    await expect.poll(() => focused(page)).toBe("file-lantern-glow-1.2.0.zip");
    await pad(page, "a");
    await expect(page.locator(".deck-toast")).toContainText("Updated Lantern Glow: 1.1.0 → 1.2.0", { timeout: 8000 });
    await expect(page.locator(".deck-toast")).not.toContainText("Added Lantern Glow");
  });
});
