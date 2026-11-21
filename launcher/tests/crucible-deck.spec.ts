// Crucible on the Deck (src/crucible/DeckCrucible.tsx; DESIGN 2) at 1280×800 in mock mode, driven by the pad: Share
// to Crucible on Home (the first share's confirm, then the upload, then the link's QR code and the big code), Enter a
// code (Open), the preview with focus on Install and play, its check line, the trust row, and the install.
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";

test.use({ viewport: { width: 1280, height: 800 } });
const SHOTS = process.env.KILN_CRUCIBLE_SHOTS;
const shot = async (page: Page, name: string) => { if (SHOTS) { mkdirSync(SHOTS, { recursive: true }); await page.screenshot({ path: `${SHOTS}/${name}.png` }); } };

async function pad(page: Page, button: string) {
  const send = (pressed: boolean) => page.evaluate(([b, p]) => (window as unknown as { __kilnMock: { emit: (n: string, v: unknown) => void } }).__kilnMock
    .emit("kiln://gamepad", { kind: "button", button: b, pressed: p, repeat: false, device: "event9" }), [button, pressed] as const);
  await send(true);
  await send(false);
  await page.waitForTimeout(80);
}
const focused = (page: Page) => page.evaluate(() => (document.activeElement as HTMLElement | null)?.dataset?.deckId ?? null);
async function focusOn(page: Page, id: string) {
  await page.locator(`[data-deck-id="${id}"]`).waitFor();
  await page.locator(`[data-deck-id="${id}"]`).focus();
  await expect.poll(() => focused(page)).toBe(id);
}

async function openDeck(page: Page, url: string) {
  await page.goto(url);
  await expect(page.locator('[data-deck-screen="home"]')).toBeVisible();
  await expect.poll(() => focused(page)).toBe("mouth");
}

test("Share to Crucible: the confirm, then the QR code and the big code", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck&discord=in#/");
  await focusOn(page, "crucible-share");
  await pad(page, "a");
  const confirm = page.getByTestId("deck-crucible-confirm");
  await expect(confirm).toContainText("Goes up: the mod list");
  await expect(confirm).toContainText("Never sent: your saves, worlds, characters or any other personal file.");
  await shot(page, "deck-share-confirm");
  await page.waitForTimeout(300);
  expect(await page.evaluate(() => (window as unknown as { __kilnCrucibleMock: { requests: () => { cmd: string }[] } }).__kilnCrucibleMock.requests().filter((r) => r.cmd === "crucible_share").length)).toBe(0);
  await focusOn(page, "crucible-share-confirm");
  await pad(page, "a");
  await expect(page.getByTestId("deck-crucible-uploading")).toBeVisible();
  await expect(page.getByTestId("deck-crucible-code")).toHaveText("H7QK-3MZP", { timeout: 15_000 });
  await expect(page.locator('[data-deck-dialog="crucible-share"] svg')).toBeVisible();
  await shot(page, "deck-share");
  await pad(page, "b");
  await expect(page.locator('[data-deck-dialog="crucible-share"]')).toHaveCount(0);
});

test("Share signed out: the sign-in comes first", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck#/");
  await focusOn(page, "crucible-share");
  await pad(page, "a");
  await expect(page.getByTestId("crucible-share-signin-first")).toHaveText("Sign in with Discord to share. Your friends don't need to.");
  await focusOn(page, "crucible-share-confirm");
  await expect(page.locator('[data-deck-id="crucible-share-confirm"]')).toHaveText("Sign in and share");
  await pad(page, "a");
  await expect(page.getByTestId("deck-qr")).toBeVisible();
});

test("Enter a code: the preview with focus on Install and play, trust, install", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck#/");
  await focusOn(page, "crucible-code");
  await pad(page, "a");
  const input = page.getByTestId("deck-crucible-code-input");
  await expect(input).toBeFocused();
  await input.pressSequentially("k4tw9xrb");
  await expect(input).toHaveValue("K4TW-9XRB");
  await input.press("Enter");
  await expect(page.getByTestId("deck-crucible-preview")).toContainText("by Cinderhawk");
  await expect(page.getByTestId("deck-crucible-checks")).toHaveText("Checks: passed");
  await expect.poll(() => focused(page)).toBe("crucible-install");
  await expect(page.getByTestId("deck-crucible-warning")).toContainText("This pack contains mods, including code, from other people.");
  await shot(page, "deck-preview");
  // not ticked yet: A says what is missing
  await pad(page, "a");
  await expect(page.getByTestId("deck-crucible-note")).toHaveText('Tick "I trust" first.');
  await focusOn(page, "crucible-trust");
  await pad(page, "a");
  await focusOn(page, "crucible-install");
  await pad(page, "a");
  await expect(page.locator('[data-deck-dialog="crucible-done"]')).toContainText("Cinder Trails (Cinderhawk's pack) is installed", { timeout: 15_000 });
  await shot(page, "deck-installed");
  await focusOn(page, "crucible-undo");
  await pad(page, "a");
  await expect(page.locator('[data-deck-dialog="crucible-done"]')).toHaveCount(0);
  await expect(page.getByTestId("deck-profile")).toContainText("Ember run");
});

test("a kiln:// link opens the Deck preview", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck#/");
  await page.evaluate(() => (window as unknown as { __kilnCrucibleMock: { link: (u: string) => void } }).__kilnCrucibleMock.link("https://frostdev.io/kiln/crucible/p/K4TW-9XRB"));
  await expect(page.locator('[data-deck-dialog="crucible-preview"]')).toContainText("Cinder Trails");
});
