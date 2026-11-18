// The Community page (desktop) and tab (Deck) in mock mode (src/community/, docs/kiln/launcher/discord.md): signed
// out, signed in, offline and relay down; the reply count in the header and on the tab; Join (a link on the desktop, a
// QR code on the Deck); "Show the Community page" in Settings; and what reaches the relay: nothing until the page is
// opened, then only the public news, while signed out with Rich Presence off.
import { expect, test, type Page } from "@playwright/test";

type Hooks = { community: { requests: () => string[]; reply: () => void; setOnline: (on: boolean) => void }; opened: () => string[] };
const requests = (page: Page) => page.evaluate(() => (window as unknown as { __kilnMock: Hooks }).__kilnMock.community.requests());
const opened = (page: Page) => page.evaluate(() => (window as unknown as { __kilnMock: Hooks }).__kilnMock.opened());

async function open(page: Page, url: string) {
  await page.goto(url);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}
const nav = (page: Page) => page.getByRole("navigation", { name: "Main" });

test.describe("desktop", () => {
  test("signed out, presence off: nothing reaches the relay until the page opens, then only the public news", async ({ page }) => {
    await open(page, "/?mock=1#/");
    await expect(nav(page).getByRole("link", { name: "Community", exact: true })).toBeVisible();
    // the launcher runs, the player visits other pages: no Discord-related request
    for (const r of ["/logs", "/settings", "/"]) { await page.goto(`/?mock=1#${r}`); await page.waitForTimeout(300); }
    await page.waitForTimeout(800);
    expect(await requests(page)).toEqual([]);
    await nav(page).getByRole("link", { name: "Community", exact: true }).click();
    await expect(page.locator(".k-page-head")).toContainText("Kiln's Discord: help from the Kiln team and from other players. All of it is optional.");
    await expect(page.getByTestId("kc-news")).toContainText("Kiln 0.2.0-dev.29.18");
    await expect(page.getByTestId("kc-events")).toContainText("Community night: show your bases");
    expect(await requests(page)).toEqual(["feed", "known-issues"]);
  });

  test("signed out: the page says what it is, Join opens the invite, signing in is in the aside", async ({ page }) => {
    await open(page, "/?mock=1#/community");
    await expect(page.getByTestId("kc-posts")).toContainText("Sign in to see your posts and their replies here.");
    await expect(page.getByTestId("kc-help")).toContainText("Sending needs a Discord sign-in. You can write the report first.");
    await expect(page.getByTestId("kc-account")).toContainText("Not signed in.");
    await expect(page.getByTestId("kc-signin")).toBeEnabled();
    await expect(page.getByTestId("nav-community-count")).toHaveCount(0);
    await page.getByTestId("kc-join").click();
    await expect.poll(() => opened(page)).toContain("https://discord.gg/7pjr8SMTfU");
    await expect(page.getByTestId("kc-join")).toHaveText("Join the Kiln Discord");
  });

  test("signed in: the name, the membership, the posts with their states and the reply count", async ({ page }) => {
    await open(page, "/?mock=1&discord=in#/community");
    await expect(page.locator(".k-page-head")).toContainText("Signed in as emberfox. 1 new reply from Kiln support.");
    await expect(page.locator(".k-page-meta")).toContainText("In the Kiln Discord");
    await expect(page.getByTestId("kc-join")).toHaveText("Open the Kiln Discord");
    const count = page.getByTestId("nav-community-count");
    await expect(count).toHaveText("1");
    await expect(count).toHaveAttribute("aria-label", "1 new reply from Kiln support");
    const posts = page.getByTestId("kc-post");
    await expect(posts).toHaveCount(3);
    await expect(posts.nth(0)).toContainText("New reply");
    await expect(posts.nth(0)).toContainText("Rowan of the Kiln team replied 25 minutes ago. 3 replies.");
    await expect(posts.nth(1)).toContainText("Waiting");
    await expect(posts.nth(2)).toContainText("Solved");
    // opening the post with the new reply marks it seen: the count goes
    await posts.nth(0).getByRole("button", { name: "Open in Discord" }).click();
    await expect.poll(() => opened(page)).toContain("https://discord.com/channels/1568454645683195976/15731308842100003");
    await expect(count).toHaveCount(0);
    await expect(posts.nth(0)).toContainText("Replied");
    // the account and the posts were read when the page opened (signed in: allowed)
    expect(await requests(page)).toEqual(expect.arrayContaining(["me", "posts", "feed"]));
  });

  test("a new reply raises the count while the launcher runs", async ({ page }) => {
    await open(page, "/?mock=1&discord=empty#/");
    await expect(page.getByTestId("nav-community-count")).toHaveCount(0);
    await page.goto("/?mock=1&discord=empty#/community");
    await expect(page.getByTestId("kc-posts")).toContainText("No posts yet. A report you send shows here with its replies.");
  });

  test("not in the server yet: the word says so and the button joins", async ({ page }) => {
    await open(page, "/?mock=1&discord=notmember#/community");
    await expect(page.locator(".k-page-meta")).toContainText("Not in the Kiln Discord yet");
    await expect(page.getByTestId("kc-join")).toHaveText("Join the Kiln Discord");
    await expect(page.getByTestId("kc-account")).toContainText("Not in the Kiln Discord yet");
  });

  test("offline signed out: the page needs the internet, sign-in and Join are off", async ({ page }) => {
    await open(page, "/?mock=1&discord=offline#/community");
    await expect(page.getByTestId("kc-news")).toContainText("News loads when you're back online.");
    await expect(page.locator(".k-page-head")).toContainText("You're offline. This page needs the internet.");
    await expect(page.getByTestId("kc-signin")).toBeDisabled();
    await expect(page.getByTestId("kc-join")).toBeDisabled();
  });

  test("offline signed in: the posts as they were, with the time", async ({ page }) => {
    await open(page, "/?mock=1&discord=in,offline#/community");
    await expect(page.locator(".k-page-head")).toContainText(/You're offline\. Your posts show as they were at \d\d:\d\d( [AP]M)?\./);
    await expect(page.getByTestId("kc-posts")).toContainText(/As of \d\d:\d\d/);
  });

  test("the relay down, signed in: Kiln support isn't answering", async ({ page }) => {
    await open(page, "/?mock=1&discord=in,relaydown#/community");
    await expect(page.locator(".k-page-head")).toContainText(/Kiln support isn't answering right now\. Your posts show as they were at/);
  });

  test("Show the Community page off: no page, no count, no request; on again in Settings", async ({ page }) => {
    await open(page, "/?mock=1&discord=hidden,in#/community");
    // the route goes to Play
    await expect(page).toHaveURL(/#\/$/);
    await expect(nav(page).getByRole("link", { name: "Community", exact: true })).toHaveCount(0);
    await expect(page.getByTestId("nav-community-count")).toHaveCount(0);
    await page.goto("/?mock=1&discord=hidden,in#/settings");
    const toggle = page.getByTestId("community-shown").getByRole("switch");
    await expect(toggle).not.toBeChecked();
    await page.waitForTimeout(500);
    expect(await requests(page)).toEqual([]);
    await toggle.click({ force: true });
    await expect(nav(page).getByRole("link", { name: "Community", exact: true })).toBeVisible();
    await expect(page.getByTestId("nav-community-count")).toHaveText("1");
    await toggle.click({ force: true });
    await expect(nav(page).getByRole("link", { name: "Community", exact: true })).toHaveCount(0);
  });

  test("at 1024x640 the page still fits its two columns", async ({ page }) => {
    await page.setViewportSize({ width: 1024, height: 640 });
    await open(page, "/?mock=1&discord=in#/community");
    await expect(page.getByTestId("kc-account")).toBeVisible();
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
    expect(overflow).toBeLessThanOrEqual(0);
  });
});

test.describe("Steam Deck", () => {
  test.use({ viewport: { width: 1280, height: 800 } });
  const screen = (page: Page) => page.evaluate(() => document.querySelector("[data-deck-screen]")?.getAttribute("data-deck-screen") ?? null);
  const focused = (page: Page) => page.evaluate(() => (document.activeElement as HTMLElement | null)?.dataset?.deckId ?? null);

  test("the Community tab sits between Editor and Help, with the reply count", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck&discord=in#/deck");
    await expect(page.locator('[data-deck-screen="home"]')).toBeVisible();
    const tabs = page.locator(".deck-tab");
    const names = await tabs.allInnerTexts();
    expect(names.map((n) => n.replace(/\d+$/, "").trim())).toEqual(["Home", "Mods", "Add mods", "Catalog", "Kiln", "Editor", "Community", "Help", "Settings"]);
    await expect(page.locator(".deck-tab .kc-nav-count")).toHaveText("1");
  });

  test("signed out: Send first, sign-in by phone, Join shows a QR code of the invite", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck#/deck/community");
    await expect.poll(() => screen(page)).toBe("community");
    await expect(page.locator(".deck-say")).toContainText("Help from the Kiln team on Discord. Optional.");
    await expect.poll(() => focused(page)).toBe("send");
    await expect(page.locator('[data-deck-id="signin"]')).toContainText("With your phone: scan a code");
    await page.locator('[data-deck-id="join"]').click();
    const qr = page.getByTestId("deck-qr-link");
    await expect(qr).toBeVisible();
    await expect(qr.locator("svg")).toHaveAttribute("data-qr", "https://discord.gg/7pjr8SMTfU");
    await expect(qr).toContainText("discord.gg/7pjr8SMTfU");
  });

  test("signed in: the account row and the posts; a post shows its QR code and marks its replies seen", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck&discord=in#/deck/community");
    await expect(page.locator('[data-deck-id="account"]')).toContainText("emberfox");
    await expect(page.locator('[data-deck-id="account"]')).toContainText("Signed in. In the Kiln Discord.");
    await page.locator('[data-deck-id="post-p3"]').click();
    const post = page.getByTestId("deck-post");
    await expect(post).toContainText("New reply");
    await expect(post.locator("svg")).toHaveAttribute("data-qr", "https://discord.com/channels/1568454645683195976/15731308842100003");
    await expect(page.locator(".deck-tab .kc-nav-count")).toHaveCount(0);
  });

  test("hidden in Settings: no Community tab", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck&discord=hidden#/deck");
    await expect(page.locator('[data-deck-screen="home"]')).toBeVisible();
    await expect(page.locator(".deck-tab", { hasText: "Community" })).toHaveCount(0);
    await page.goto("/?mock=1&deck=steamdeck&discord=hidden#/deck/settings");
    const row = page.locator('[data-deck-id="set-community"]');
    await expect(row).toContainText("Off");
    await row.click();
    await expect(page.locator(".deck-tab", { hasText: "Community" })).toBeVisible();
  });
});

test.describe("Rich Presence", () => {
  test("off by default; on shows the preview lit and asks the relay for nothing", async ({ page }) => {
    await open(page, "/?mock=1#/community");
    const card = page.getByTestId("kc-presence");
    await expect(card.getByRole("switch")).not.toBeChecked();
    await expect(page.getByTestId("kc-presence-card")).toHaveClass(/dim/);
    await expect(page.getByTestId("kc-presence-card")).toContainText("Enshrouded with Kiln");
    await expect(page.getByTestId("kc-presence-card")).toContainText(/\d+ mods? on/);
    await expect(card).toContainText("Never shown: server names, world names, your character, or which mods you use.");
    const before = (await requests(page)).length;
    await card.getByRole("switch").click({ force: true });
    await expect(page.locator(".k-toast").filter({ hasText: "Rich Presence is on" })).toBeVisible();
    await expect(page.getByTestId("kc-presence-state")).toHaveText("On. It shows while you play.");
    await expect(page.getByTestId("kc-presence-card")).not.toHaveClass(/dim/);
    expect((await requests(page)).length).toBe(before);
    await card.getByRole("switch").click({ force: true });
    await expect(page.locator(".k-toast").filter({ hasText: "Rich Presence is off" })).toBeVisible();
    await expect(page.getByTestId("kc-presence-state")).toHaveCount(0);
  });

  test("Discord not running: the page says so", async ({ page }) => {
    await open(page, "/?mock=1&discord=presence,nodiscord#/community");
    await expect(page.getByTestId("kc-presence-state")).toHaveText("Discord isn't running on this PC. It shows once Discord is open.");
  });

  test("the Deck's dialog: the preview, on this Deck, Turn on and off", async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.goto("/?mock=1&deck=steamdeck&discord=nodiscord#/deck/community");
    const row = page.locator('[data-deck-id="presence"]');
    await expect(row).toContainText("Off");
    await row.click();
    const d = page.getByTestId("deck-presence");
    await expect(d).toContainText("Off.");
    await page.locator('[data-deck-dialog="community-presence"]').getByRole("button", { name: "Turn on" }).click();
    await expect(d).toContainText("Discord isn't running on this Deck. It shows once Discord is open.");
    await page.locator('[data-deck-dialog="community-presence"]').getByRole("button", { name: "Turn off" }).click();
    await expect(d).toContainText("Off.");
  });
});
