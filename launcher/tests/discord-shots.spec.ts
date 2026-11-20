// Screenshots of every Discord screen and state, desktop and Deck, in mock mode (docs/kiln/launcher/discord.md), named
// as the approved prototype's (dev29/discord-prototype) so the two sets can be laid side by side. Only with SHOTS set:
//   SHOTS=<dir> KILN_DEV_PORT=<port> bun x playwright test tests/discord-shots.spec.ts
// The flows themselves are tested in community*.spec.ts; this file only draws them.
import { expect, test, type Page } from "@playwright/test";

const OUT = process.env.SHOTS ?? "";
test.skip(!OUT, "SHOTS is not set");
test.use({ viewport: { width: 1280, height: 800 } });
test.describe.configure({ mode: "serial" });

async function go(page: Page, query: string, hash: string) {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto(`/?mock=1${query ? `&${query}` : ""}#${hash}`);
  await page.waitForTimeout(1600);
}
const shot = (page: Page, name: string) => page.screenshot({ path: `${OUT}/${name}.png` });
const modal = (page: Page) => page.locator(".k-modal");
/** The mock's seeded "Refused" banner is dismissed, so the shot shows only the problem it is about. */
async function noRefusal(page: Page) {
  const b = page.locator(".k-banner", { hasText: "Refused" }).getByRole("button", { name: "Dismiss" });
  if (await b.count()) await b.click();
}

async function writeReport(page: Page, line: string, more = "") {
  await page.locator("#kc-line").fill(line);
  if (more) await page.locator("#kc-more").fill(more);
  await modal(page).getByRole("button", { name: "Next: check what is sent" }).click();
  await page.waitForTimeout(400);
}
async function sendFromCommunity(page: Page, flags: string) {
  await go(page, `discord=${flags}`, "/community");
  await page.getByRole("button", { name: "Send a report" }).click();
  await page.waitForTimeout(500);
  await writeReport(page, "Mods don't load since the game update", "It worked yesterday. I turned off Night Sky and it still fails.");
  await modal(page).getByRole("button", { name: "Send to Kiln support" }).click();
  await page.waitForTimeout(500);
}

// ---------------------------------------------------------------- desktop

test("desktop: Community page, signed out, sign-in round trip", async ({ page }) => {
  await go(page, "", "/community");
  await shot(page, "desktop-01-community-signed-out");
  await page.locator(".kc-page").evaluate((el) => el.scrollTo(0, el.scrollHeight));
  await page.waitForTimeout(300);
  await shot(page, "desktop-02-community-signed-out-lower");
  await page.getByRole("button", { name: "Sign in with Discord" }).click();
  await page.waitForTimeout(400);
  await shot(page, "desktop-03-signin-start");
  await modal(page).getByRole("button", { name: "Open the sign-in page" }).click();
  await page.waitForTimeout(600);
  await shot(page, "desktop-04-signin-waiting-for-browser");
  await page.waitForTimeout(3800);
  await shot(page, "desktop-05-signed-in-toast");
});

test("desktop: sign-in errors", async ({ page }) => {
  await go(page, "discord=cancel", "/community");
  await page.getByRole("button", { name: "Sign in with Discord" }).click();
  await modal(page).getByRole("button", { name: "Open the sign-in page" }).click();
  await page.waitForTimeout(4400);
  await shot(page, "desktop-06-signin-cancelled");
  await go(page, "discord=offline", "/community");
  await shot(page, "desktop-07-community-offline-signed-out");
});

test("desktop: signed in, posts, reply badge, menu, delete", async ({ page }) => {
  await go(page, "discord=in,presence", "/community");
  await expect(page.getByTestId("nav-community-count")).toHaveText("1");
  await shot(page, "desktop-08-community-signed-in-new-reply-badge");
  await page.getByRole("button", { name: /^More for Mods don't load/ }).click();
  await page.waitForTimeout(300);
  await shot(page, "desktop-09-post-menu");
  await page.getByRole("menuitem", { name: "Delete my post" }).click();
  await page.waitForTimeout(500);
  await shot(page, "desktop-10-delete-post-confirm");
  await go(page, "discord=in,offline", "/community");
  await shot(page, "desktop-11-community-offline-signed-in");
  await go(page, "discord=empty", "/community");
  await shot(page, "desktop-12-community-signed-in-no-posts");
});

test("desktop: send from the Logs page, signed in", async ({ page }) => {
  await go(page, "discord=in", "/logs");
  await shot(page, "desktop-13-logs-send-to-kiln-support");
  await page.getByTestId("send-to-support").click();
  await page.waitForTimeout(600);
  await shot(page, "desktop-14-send-write-empty");
  await page.locator("#kc-line").fill("Mods don't load since the game update");
  await page.locator("#kc-more").fill("It worked yesterday. I turned off Night Sky and it still fails.");
  await shot(page, "desktop-15-send-write-filled");
  await modal(page).getByRole("button", { name: "Next: check what is sent" }).click();
  await page.waitForTimeout(500);
  await shot(page, "desktop-16-send-check-what-is-sent");
  await modal(page).getByRole("button", { name: "Show the files" }).click();
  await page.locator(".k-modal-body").evaluate((el) => el.scrollTo(0, el.scrollHeight));
  await page.waitForTimeout(200);
  await shot(page, "desktop-17-send-check-files-and-who-sees-the-zip");
  await modal(page).getByRole("button", { name: "Send to Kiln support" }).click();
  await page.waitForTimeout(600);
  await shot(page, "desktop-18-send-sent");
  await modal(page).getByRole("button", { name: "Close" }).first().click();
  await page.goto("/?mock=1&discord=in#/community");
  await page.waitForTimeout(100);
});

test("desktop: send signed out keeps the draft through sign-in", async ({ page }) => {
  await go(page, "", "/logs");
  await page.getByTestId("send-to-support").click();
  await page.waitForTimeout(400);
  await writeReport(page, "The launcher shows a black window", "Since I updated my graphics driver.");
  await shot(page, "desktop-19-send-check-signed-out");
  await modal(page).getByRole("button", { name: "Sign in with Discord to send" }).click();
  await page.waitForTimeout(300);
  await modal(page).getByRole("button", { name: "Open the sign-in page" }).click();
  await page.waitForTimeout(4400);
  await shot(page, "desktop-20-send-check-after-signin-draft-kept");
});

test("desktop: problem banners on Play, known problems", async ({ page }) => {
  await go(page, "discord=in&problem=nokiln", "/");
  await noRefusal(page);
  await page.getByRole("button", { name: "Without mods" }).click();
  await expect(page.getByText("Kiln is not loading")).toBeVisible({ timeout: 15_000 });
  await page.waitForTimeout(600);
  await shot(page, "desktop-21-play-banner-kiln-not-loading");
  await page.locator(".k-banner", { hasText: "Kiln is not loading" }).getByRole("button", { name: "Send to Kiln support" }).click();
  await page.waitForTimeout(700);
  await shot(page, "desktop-22-send-known-problem-launch-option");
  await page.evaluate(() => (window as unknown as { __kilnMock: { stopGame: () => void } }).__kilnMock.stopGame());
  await go(page, "discord=in&problem=crash", "/");
  await noRefusal(page);
  await expect(page.getByTestId("crash-notice")).toBeVisible({ timeout: 10_000 });
  await page.waitForTimeout(600);
  await shot(page, "desktop-23-play-crash-notice");
  await page.locator(".k-banner", { hasText: "The game closed unexpectedly" }).getByRole("button", { name: "Send to Kiln support" }).click();
  await page.waitForTimeout(700);
  await shot(page, "desktop-24-send-known-problem-crash");
});

test("desktop: send errors", async ({ page }) => {
  await sendFromCommunity(page, "in,relaydown");
  await shot(page, "desktop-25-send-error-relay-down");
  await sendFromCommunity(page, "notmember");
  await shot(page, "desktop-26-send-error-not-in-the-server");
  await sendFromCommunity(page, "in,ratelimit");
  await shot(page, "desktop-27-send-error-too-many-reports");
  await sendFromCommunity(page, "in,toobig");
  await shot(page, "desktop-28-send-error-report-too-big");
  await go(page, "discord=in,offline", "/community");
  await page.getByRole("button", { name: "Send a report" }).click();
  await page.waitForTimeout(400);
  await writeReport(page, "Mods don't load since the game update");
  await shot(page, "desktop-29-send-offline");
});

test("desktop: presence, privacy, forget, reply notification", async ({ page }) => {
  await go(page, "discord=in,presence,nodiscord", "/community");
  await page.getByTestId("kc-presence").scrollIntoViewIfNeeded();
  await shot(page, "desktop-30-presence-on-discord-not-running");
  await page.getByTestId("kc-privacy-link").scrollIntoViewIfNeeded();
  await page.getByTestId("kc-privacy-link").click();
  await page.waitForTimeout(400);
  await shot(page, "desktop-31-what-is-sent-and-kept");
  await modal(page).getByRole("button", { name: "Close" }).last().click();
  await page.waitForTimeout(300);
  await page.getByRole("button", { name: "Sign out and forget me" }).click();
  await page.waitForTimeout(500);
  await shot(page, "desktop-32-sign-out-and-forget");
  await go(page, "discord=in,notify", "/");
  await noRefusal(page);
  await page.evaluate(() => (window as unknown as { __kilnMock: { community: { reply: () => void } } }).__kilnMock.community.reply());
  await page.waitForTimeout(700);
  await shot(page, "desktop-33-reply-notification-and-badge");
});

test("desktop: testers", async ({ page }) => {
  await go(page, "discord=in", "/community");
  await page.getByRole("button", { name: "Volunteer as a tester" }).scrollIntoViewIfNeeded();
  await shot(page, "desktop-34-tester-pitch");
  await page.getByRole("button", { name: "Volunteer as a tester" }).click();
  await page.waitForTimeout(400);
  await shot(page, "desktop-35-tester-volunteer-hardware");
  await modal(page).getByRole("button", { name: "Volunteer" }).click();
  await page.waitForTimeout(800);
  await page.getByTestId("kc-checklist").scrollIntoViewIfNeeded();
  await shot(page, "desktop-36-tester-checklist-new");
  await go(page, "discord=tester", "/community");
  const list = page.getByTestId("kc-checklist");
  await list.scrollIntoViewIfNeeded();
  await list.getByRole("radio", { name: "Worked" }).click();
  await page.waitForTimeout(300);
  await list.getByRole("button", { name: /Turn on FSR 4/ }).click();
  await list.getByRole("radio", { name: "Didn't work" }).click();
  await list.getByPlaceholder("What happened?").fill("Water smears when I turn the camera");
  await list.getByPlaceholder("What happened?").blur();
  await page.waitForTimeout(300);
  await list.scrollIntoViewIfNeeded();
  await shot(page, "desktop-37-tester-checklist-results");
  await go(page, "discord=sent", "/community");
  await page.getByTestId("kc-checklist").scrollIntoViewIfNeeded();
  await shot(page, "desktop-38-tester-results-sent-not-read");
  await go(page, "discord=read", "/community");
  await page.getByTestId("kc-checklist").scrollIntoViewIfNeeded();
  await shot(page, "desktop-39-tester-results-read");
});

test("desktop: the header at the smallest window", async ({ page }) => {
  await page.setViewportSize({ width: 1024, height: 640 });
  await go(page, "discord=in", "/community");
  await shot(page, "desktop-40-community-at-1024x640");
});

// ---------------------------------------------------------------- Steam Deck

const deck = (page: Page, flags: string, hash = "/deck/community") => go(page, `deck=steamdeck${flags ? `&discord=${flags}` : ""}`, hash);
const dialog = (page: Page) => page.locator(".deck-dialog");

test("deck: Community tab, QR sign-in", async ({ page }) => {
  await deck(page, "");
  await shot(page, "deck-01-community-signed-out");
  await page.getByRole("button", { name: /Sign in with Discord/ }).click();
  await page.waitForTimeout(900);
  await shot(page, "deck-02-qr-signin-waiting");
  await page.waitForTimeout(9000);
  await shot(page, "deck-03-qr-signin-approved-signed-in");
  await deck(page, "expire");
  await page.getByRole("button", { name: /Sign in with Discord/ }).click();
  await page.waitForTimeout(10500);
  await shot(page, "deck-04-qr-code-expired");
});

test("deck: signed in, posts", async ({ page }) => {
  await deck(page, "in");
  await expect(page.getByTestId("nav-community-count")).toHaveText("1");
  await shot(page, "deck-05-community-signed-in-new-reply");
  await page.getByRole("button", { name: /Mods don't load after the game update/ }).click();
  await page.waitForTimeout(700);
  await shot(page, "deck-06-post-open-on-phone");
});

test("deck: send flow with the gamepad", async ({ page }) => {
  await deck(page, "in");
  await page.getByRole("button", { name: "Send to Kiln support" }).first().click();
  await page.waitForTimeout(600);
  await shot(page, "deck-07-send-what-went-wrong");
  await dialog(page).getByRole("button", { name: "The game crashes", exact: true }).click();
  await page.waitForTimeout(500);
  await shot(page, "deck-08-send-when");
  await dialog(page).getByRole("button", { name: /when I load a world/ }).click();
  await page.waitForTimeout(600);
  await shot(page, "deck-09-send-check-and-send");
  await dialog(page).getByRole("button", { name: /None\. Optional\./ }).click();
  await page.waitForTimeout(500);
  await page.locator(".kdc-input").fill("Only on my big base world");
  await shot(page, "deck-10-send-type-with-steam-keyboard");
  await dialog(page).getByRole("button", { name: "Done" }).click();
  await page.waitForTimeout(500);
  await dialog(page).getByRole("button", { name: /What is sent/ }).click();
  await page.waitForTimeout(500);
  await shot(page, "deck-11-send-what-is-sent");
  await dialog(page).getByRole("button", { name: "OK" }).click();
  await page.waitForTimeout(400);
  await dialog(page).getByRole("button", { name: "Send to Kiln support" }).click();
  await page.waitForTimeout(800);
  await shot(page, "deck-12-send-sent-qr");
});

test("deck: send signed out, offline, too many", async ({ page }) => {
  await deck(page, "");
  await page.getByRole("button", { name: "Send to Kiln support" }).first().click();
  await dialog(page).getByRole("button", { name: "Kiln doesn't load", exact: true }).click();
  await dialog(page).getByRole("button", { name: /when I press Play/ }).click();
  await page.waitForTimeout(500);
  await shot(page, "deck-13-send-check-signed-out");
  await deck(page, "in,offline");
  await page.getByRole("button", { name: "Send to Kiln support" }).first().click();
  await dialog(page).getByRole("button", { name: "Kiln doesn't load", exact: true }).click();
  await dialog(page).getByRole("button", { name: /when I press Play/ }).click();
  await page.waitForTimeout(500);
  await shot(page, "deck-14-send-offline");
  await deck(page, "in,ratelimit");
  await page.getByRole("button", { name: "Send to Kiln support" }).first().click();
  await dialog(page).getByRole("button", { name: "Kiln doesn't load", exact: true }).click();
  await dialog(page).getByRole("button", { name: /when I press Play/ }).click();
  await dialog(page).getByRole("button", { name: "Send to Kiln support" }).click();
  await page.waitForTimeout(600);
  await shot(page, "deck-15-send-too-many-reports");
});

test("deck: Home problem with a known fix", async ({ page }) => {
  await go(page, "deck=steamdeck&discord=in&problem=crash", "/deck");
  await expect(page.getByTestId("deck-crash")).toBeVisible({ timeout: 10_000 });
  await page.waitForTimeout(600);
  await shot(page, "deck-16-home-crash-send-to-kiln-support");
  await page.getByRole("button", { name: "Send to Kiln support" }).click();
  await dialog(page).getByRole("button", { name: "The game crashes", exact: true }).click();
  await dialog(page).getByRole("button", { name: /while I play/ }).click();
  await page.waitForTimeout(600);
  await shot(page, "deck-17-send-check-known-problem");
  await dialog(page).getByRole("button", { name: /Night Sky and Weather Plus/ }).click();
  await page.waitForTimeout(500);
  await shot(page, "deck-18-known-problem-fix");
});

test("deck: join, presence, privacy", async ({ page }) => {
  await deck(page, "in,presence");
  await page.getByRole("button", { name: /Open the Kiln Discord/ }).click();
  await page.waitForTimeout(600);
  await shot(page, "deck-19-join-qr");
  await dialog(page).getByRole("button", { name: "Done" }).click();
  await page.waitForTimeout(400);
  await page.getByRole("button", { name: /Rich Presence/ }).click();
  await page.waitForTimeout(600);
  await shot(page, "deck-20-rich-presence");
  await page.keyboard.press("Escape");
  await page.waitForTimeout(400);
  await page.getByRole("button", { name: /What is sent and kept/ }).click();
  await page.waitForTimeout(600);
  await shot(page, "deck-21-what-is-sent-and-kept");
});

test("deck: testers", async ({ page }) => {
  await deck(page, "in");
  await page.getByRole("button", { name: /Help test Kiln/ }).click();
  await page.waitForTimeout(600);
  await shot(page, "deck-22-tester-volunteer-hardware");
  await deck(page, "tester");
  await page.getByRole("button", { name: /Test 0\.2\.0-dev\.30/ }).click();
  await page.waitForTimeout(600);
  await shot(page, "deck-23-tester-checklist");
  await dialog(page).getByRole("button", { name: /Play on battery/ }).click();
  await page.waitForTimeout(600);
  await shot(page, "deck-24-tester-step-result");
  await dialog(page).getByRole("button", { name: "Didn't work" }).click();
  await page.waitForTimeout(600);
  await shot(page, "deck-25-tester-what-happened-pick");
  await deck(page, "read");
  await page.getByRole("button", { name: /Test 0\.2\.0-dev\.30/ }).click();
  await page.waitForTimeout(600);
  await shot(page, "deck-26-tester-results-read");
});
