// Signing in with Discord in mock mode (src/community/SignIn.tsx; WIREFRAMES D5, K3): the desktop's browser round trip
// through frostdev.io and its endings (approved, cancelled, timed out, offline, frostdev.io down), the Deck's QR
// pairing (approved on the phone, expired with a new code, declined), sign out, and sign out and forget me.
import { expect, test, type Page } from "@playwright/test";

type Hooks = { community: { requests: () => string[]; signins: () => { mode: string; join: boolean }[]; approve: () => void; setOnline: (on: boolean) => void; setRelay: (up: boolean) => void }; opened: () => string[] };
const mock = <T,>(page: Page, f: (h: Hooks) => T) => page.evaluate((src) => (new Function("h", `return (${src})(h)`))((window as unknown as { __kilnMock: Hooks }).__kilnMock), f.toString()) as Promise<T>;
const requests = (page: Page) => mock(page, (h) => h.community.requests());
const opened = (page: Page) => mock(page, (h) => h.opened());

async function open(page: Page, url: string) {
  await page.goto(url);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

test.describe("desktop", () => {
  test("the browser round trip: what Kiln gets, Also join ticked, waiting, then signed in", async ({ page }) => {
    await open(page, "/?mock=1#/community");
    await page.getByTestId("kc-signin").click();
    const d = page.getByTestId("kc-signin-dialog");
    await expect(d).toContainText("Your browser opens Discord's sign-in page through frostdev.io. You approve Kiln there, then come back here.");
    await expect(d).toContainText("Kiln gets your Discord name, ID and picture. Nothing else: not your email, your messages, your friends or your servers.");
    await expect(page.getByTestId("kc-also-join")).toBeChecked();
    await d.getByTestId("kc-privacy-page").click();
    await expect.poll(() => opened(page)).toContain("https://frostdev.io/kiln/privacy");
    await page.getByTestId("kc-signin-open").click();
    await expect(page.getByTestId("kc-signin-wait")).toContainText("Approve Kiln on the page that opened. This window moves on by itself.");
    await expect.poll(() => opened(page)).toContain("https://frostdev.io/kiln/signin/s1");
    await expect(page.locator(".k-toast").filter({ hasText: "Signed in as emberfox" })).toBeVisible({ timeout: 10_000 });
    await expect(page.getByTestId("kc-signin-wait")).toHaveCount(0);
    await expect(page.getByTestId("kc-account")).toContainText("emberfox");
    expect(await requests(page)).toEqual(expect.arrayContaining(["signin", "signin-poll"]));
    expect(await mock(page, (h) => h.community.signins())).toEqual([{ mode: "browser", join: true }]);
  });

  test("cancelled on Discord's page: the dialog stays with Try again", async ({ page }) => {
    await open(page, "/?mock=1&discord=cancel#/community");
    await page.getByTestId("kc-signin").click();
    await page.getByTestId("kc-signin-open").click();
    await expect(page.getByTestId("kc-signin-error")).toHaveText("You cancelled on Discord's page. Nothing changed.", { timeout: 10_000 });
    await expect(page.getByTestId("kc-signin-open")).toHaveText("Try again");
    await expect(page.getByTestId("kc-account")).toContainText("Not signed in.");
  });

  test("timed out after 10 minutes", async ({ page }) => {
    await open(page, "/?mock=1&discord=expire#/community");
    await page.getByTestId("kc-signin").click();
    await page.getByTestId("kc-signin-open").click();
    await expect(page.getByTestId("kc-signin-error")).toHaveText("The sign-in timed out after 10 minutes. Try again.", { timeout: 10_000 });
  });

  test("offline, then frostdev.io down: each says so and Try again stays", async ({ page }) => {
    await open(page, "/?mock=1#/community");
    await page.getByTestId("kc-signin").click();
    await mock(page, (h) => h.community.setOnline(false));
    await page.getByTestId("kc-signin-open").click();
    await expect(page.getByTestId("kc-signin-error")).toHaveText("You're offline. Signing in needs the internet.");
    await mock(page, (h) => { h.community.setOnline(true); h.community.setRelay(false); });
    await page.getByTestId("kc-signin-open").click();
    await expect(page.getByTestId("kc-signin-error")).toHaveText("frostdev.io isn't answering. Try again in a few minutes.");
    await mock(page, (h) => h.community.setRelay(true));
    await page.getByTestId("kc-signin-open").click();
    await expect(page.locator(".k-toast").filter({ hasText: "Signed in as emberfox" })).toBeVisible({ timeout: 10_000 });
  });

  test("too many sign-ins from the network: when again, not \"frostdev.io isn't answering\"", async ({ page }) => {
    await open(page, "/?mock=1&discord=signinlimit#/community");
    await page.getByTestId("kc-signin").click();
    await page.getByTestId("kc-signin-open").click();
    await expect(page.getByTestId("kc-signin-error")).toHaveText("Too many sign-ins came from your network in the last hour. Try again in 42 minutes.");
  });

  test("sign out: no confirmation, the launcher holds nothing of the account", async ({ page }) => {
    await open(page, "/?mock=1&discord=in#/community");
    await page.getByTestId("kc-signout").click();
    await expect(page.locator(".k-toast").filter({ hasText: "Signed out" })).toContainText("The launcher holds nothing of your Discord account now.");
    await expect(page.getByTestId("kc-account")).toContainText("Not signed in.");
    await expect(page.getByTestId("nav-community-count")).toHaveCount(0);
    expect(await requests(page)).toContain("signout");
  });

  test("sign out and forget me: the typed word, and the posts deleted from Discord when ticked", async ({ page }) => {
    await open(page, "/?mock=1&discord=in#/community");
    await page.getByTestId("kc-forget").click();
    const arch = page.getByRole("dialog", { name: "Sign out and forget you?" });
    await expect(arch).toContainText("frostdev.io deletes your name, your ID, your hardware, the list of your posts and the Kiln team's copies of your report zips. Your posts stay on Discord until you delete them.");
    await page.getByTestId("kc-also-delete").check();
    const confirm = arch.getByRole("button", { name: "Sign out and forget me" });
    await expect(confirm).toBeDisabled();
    await arch.getByRole("textbox").fill("forget");
    await confirm.click();
    await expect(page.getByTestId("kc-account")).toContainText("Not signed in.");
    expect(await requests(page)).toContain("forget+posts");
  });
});

test.describe("forget needs frostdev.io", () => {
  test("offline or frostdev.io down: Sign out and forget me waits; a forget that frostdev.io did not hear says so", async ({ page }) => {
    await open(page, "/?mock=1&discord=in,offline#/community");
    await expect(page.getByTestId("kc-forget")).toBeDisabled();
    await expect(page.getByTestId("kc-signout")).toBeEnabled();
    await open(page, "/?mock=1&discord=in#/community");
    await page.getByTestId("kc-forget").click();
    const arch = page.getByRole("dialog", { name: "Sign out and forget you?" });
    await arch.getByRole("textbox").fill("forget");
    // frostdev.io stops answering while the dialog is open
    await mock(page, (h) => h.community.setRelay(false));
    await arch.getByRole("button", { name: "Sign out and forget me" }).click();
    const toast = page.locator(".k-toast").filter({ hasText: "Signed out on this PC only" });
    await expect(toast).toBeVisible();
    await expect(toast).toContainText("frostdev.io didn't hear it, so it still keeps what it had. Sign in again and use Sign out and forget me, or write to admin@frostdev.io.");
    // the key is gone here all the same
    await expect(page.getByTestId("kc-account")).toContainText("Not signed in.");
  });

  test("Deck: the hold to forget waits for frostdev.io", async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.goto("/?mock=1&deck=steamdeck&discord=in,relaydown#/deck/community");
    await page.locator('[data-deck-id="account"]').click();
    const d = page.locator('[data-deck-dialog="community-privacy"]');
    await expect(d.getByRole("button", { name: "Sign out and forget me" })).toBeDisabled();
    await expect(d.getByRole("button", { name: "Sign out", exact: true })).toBeEnabled();
  });
});

test.describe("Steam Deck", () => {
  test.use({ viewport: { width: 1280, height: 800 } });

  test("QR pairing: the code on both screens, approved on the phone, signed in", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck#/deck/community");
    await page.locator('[data-deck-id="signin"]').click();
    const qr = page.getByTestId("deck-qr");
    await expect(page.getByTestId("deck-qr-code")).toHaveText("KQ7M-4P2X");
    await expect(qr.locator("svg")).toHaveAttribute("data-qr", "https://frostdev.io/link/KQ7M4P2X");
    await expect(qr).toContainText("Check that the page shows KQ7M-4P2X, then press Approve.");
    await expect(qr).toContainText("No camera? Open frostdev.io/link on any device and type the code.");
    await expect(qr).toContainText(/The code works for \d+:\d\d\./);
    await mock(page, (h) => h.community.approve());
    await expect(page.locator(".deck-toast").filter({ hasText: "Signed in as emberfox" })).toBeVisible();
    await expect(page.locator('[data-deck-id="account"]')).toContainText("emberfox");
    // the Deck asks with join, so frostdev.io's phone page offers "Also join the Kiln Discord" (ticked) to a player who
    // isn't in it: the choice is made on the phone (PLAN 3.2, docs/relay-api.md)
    expect(await mock(page, (h) => h.community.signins())).toEqual([{ mode: "qr", join: true }]);
  });

  test("an expired code is struck out; X or New code makes another", async ({ page }) => {
    test.setTimeout(60_000);
    await page.goto("/?mock=1&deck=steamdeck&discord=expire#/deck/community");
    await page.locator('[data-deck-id="signin"]').click();
    const qr = page.getByTestId("deck-qr");
    await expect(qr).toHaveAttribute("data-state", "expired", { timeout: 15_000 });
    await expect(qr).toContainText("This code has expired.");
    await expect(qr.locator(".kdc-qr-gone")).toBeVisible();
    await page.locator('[data-deck-dialog="community-signin"]').getByRole("button", { name: "New code" }).click();
    await expect(page.getByTestId("deck-qr-code")).toHaveText("HV3R-9TCW");
    await expect(qr).toHaveAttribute("data-state", "waiting");
  });

  test("too many sign-ins from the network: when again", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck&discord=signinlimit#/deck/community");
    await page.locator('[data-deck-id="signin"]').click();
    await expect(page.getByTestId("deck-qr")).toHaveAttribute("data-state", "busy");
    await expect(page.getByTestId("deck-qr")).toContainText("Too many sign-ins came from your network in the last hour. Try again in 42 minutes.");
  });

  test("declined on the phone", async ({ page }) => {
    test.setTimeout(60_000);
    await page.goto("/?mock=1&deck=steamdeck&discord=deny#/deck/community");
    await page.locator('[data-deck-id="signin"]').click();
    await expect(page.getByTestId("deck-qr")).toContainText("Declined on your phone. Nothing changed.", { timeout: 15_000 });
  });

  test("the account row: what is sent and kept, Sign out", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck&discord=in#/deck/community");
    await page.locator('[data-deck-id="account"]').click();
    await expect(page.getByTestId("deck-privacy")).toContainText("Nothing is sent until you press a button on a screen that shows what goes.");
    await page.locator('[data-deck-dialog="community-privacy"]').getByRole("button", { name: "Sign out", exact: true }).click();
    await expect(page.locator('[data-deck-id="signin"]')).toBeVisible();
  });
});
