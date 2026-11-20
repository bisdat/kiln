// "What is sent and kept" in mock mode (src/community/Privacy.tsx; WIREFRAMES D14, K11): every section, the links to
// the privacy page and the terms, from the Community page and the sign-in dialog on the desktop, and on the Deck with
// the privacy page as a QR code. Its words follow https://frostdev.io/kiln/privacy (unit/community.test.ts checks
// the promises in them).
import { expect, test, type Page } from "@playwright/test";

const opened = (page: Page) => page.evaluate(() => (window as unknown as { __kilnMock: { opened: () => string[] } }).__kilnMock.opened());

test("desktop: every section, the full privacy page and the terms", async ({ page }) => {
  await page.goto("/?mock=1#/community");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await page.getByTestId("kc-privacy-link").click();
  const d = page.getByTestId("kc-privacy");
  await expect(d).toContainText("Nothing is sent until you press a button on a screen that shows what goes.");
  for (const head of ["When you sign in", "When you send a report", "Rich Presence", "When you volunteer as a tester", "What frostdev.io keeps", "Undo any of it"]) {
    await expect(d.locator(".k-label", { hasText: head })).toBeVisible();
  }
  await expect(d).toContainText("A fingerprint of your sign-in, until you sign out or for 90 days unused.");
  await expect(d).toContainText("Report zips are never stored on frostdev.io.");
  const modal = page.getByRole("dialog", { name: "What is sent and kept" });
  await modal.getByTestId("kc-privacy-page").click();
  await modal.getByTestId("kc-terms").click();
  await expect.poll(() => opened(page)).toEqual(expect.arrayContaining(["https://frostdev.io/kiln/privacy", "https://frostdev.io/kiln/terms"]));
  await modal.getByRole("button", { name: "Close" }).click();
  // from the sign-in dialog too
  await page.getByTestId("kc-signin").click();
  await page.getByTestId("kc-signin-dialog").getByRole("button", { name: "What is sent and kept" }).click();
  await expect(page.getByTestId("kc-privacy")).toBeVisible();
});

test("Steam Deck: the same words, Y shows the privacy page as a QR code", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto("/?mock=1&deck=steamdeck#/deck/community");
  await page.locator('[data-deck-id="privacy"]').click();
  const d = page.getByTestId("deck-privacy");
  await expect(d).toContainText("Report zips are never stored on frostdev.io.");
  await expect(d).toContainText("The Kiln team uses AI assistants.");
  await page.keyboard.press("y");
  const qr = page.getByTestId("deck-qr-link");
  await expect(qr.locator("svg")).toHaveAttribute("data-qr", "https://frostdev.io/kiln/privacy");
  await expect(qr).toContainText("frostdev.io/kiln/privacy");
});
