// The player's support posts in mock mode (WIREFRAMES D3, D11, D12, K10): their states, a reply arriving (the count,
// the row, the notification only when asked for), It's fixed, Delete my post, Copy link, and the Deck's post dialog.
import { expect, test, type Page } from "@playwright/test";

type Hooks = { community: { requests: () => string[]; reply: (name?: string) => void; setOnline: (on: boolean) => void }; opened: () => string[] };
const mock = <T,>(page: Page, f: (h: Hooks) => T) => page.evaluate((src) => (new Function("h", `return (${src})(h)`))((window as unknown as { __kilnMock: Hooks }).__kilnMock), f.toString()) as Promise<T>;
const requests = (page: Page) => mock(page, (h) => h.community.requests());

async function open(page: Page, url: string) {
  await page.goto(url);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}
const post = (page: Page, id: string) => page.locator(`[data-testid="kc-post"][data-post="${id}"]`);

test.describe("desktop", () => {
  test("with the Discord app on this PC, Open in Discord and Open Discord open the app's discord: links", async ({ page }) => {
    await open(page, "/?mock=1&discord=in&discordapp=1#/community");
    await post(page, "p3").getByRole("button", { name: "Open in Discord" }).click();
    await expect.poll(() => mock(page, (h) => h.opened())).toContain("discord://-/channels/1568454645683195976/15731308842100003");
    await page.getByTestId("kc-join").click();
    await expect.poll(() => mock(page, (h) => h.opened())).toContain("discord://-/channels/1568454645683195976");
    expect((await mock(page, (h) => h.opened())).filter((u) => u.startsWith("https://discord"))).toEqual([]);
  });

  test("without the Discord app, the same buttons open the web links (the server's invite for Open Discord)", async ({ page }) => {
    await open(page, "/?mock=1&discord=in#/community");
    await post(page, "p3").getByRole("button", { name: "Open in Discord" }).click();
    await page.getByTestId("kc-join").click();
    await expect.poll(() => mock(page, (h) => h.opened())).toEqual(["https://discord.com/channels/1568454645683195976/15731308842100003", "https://discord.gg/7pjr8SMTfU"]);
  });

  test("a reply arrives: the count, the row's word and line; no notification while the switch is off", async ({ page }) => {
    await open(page, "/?mock=1&discord=in#/community");
    await expect(page.getByTestId("kc-account").getByRole("switch")).not.toBeChecked();
    await post(page, "p3").getByRole("button", { name: "Open in Discord" }).click();
    await expect(page.getByTestId("nav-community-count")).toHaveCount(0);
    await mock(page, (h) => h.community.reply("Rowan"));
    await expect(page.getByTestId("nav-community-count")).toHaveText("1");
    await expect(post(page, "p3")).toContainText("New reply");
    await expect(post(page, "p3")).toContainText("Rowan of the Kiln team replied just now. 4 replies.");
    await expect(page.locator(".k-toast").filter({ hasText: "Kiln support replied" })).toHaveCount(0);
  });

  test("with Tell me when Kiln support replies on: a notice with Open in Discord", async ({ page }) => {
    await open(page, "/?mock=1&discord=in#/community");
    const sw = page.getByTestId("kc-account").getByRole("switch");
    await sw.click({ force: true });
    await expect(sw).toBeChecked();
    await mock(page, (h) => h.community.reply("Rowan"));
    const n = page.locator(".k-toast").filter({ hasText: "Kiln support replied" });
    await expect(n).toContainText("Rowan replied to “Mods don't load after the game update”.");
    await n.getByRole("button", { name: "Open in Discord" }).click();
    await expect.poll(() => mock(page, (h) => h.opened())).toContain("https://discord.com/channels/1568454645683195976/15731308842100003");
  });

  test("the menu: Copy link, It's fixed, Delete my post with the typed word", async ({ page }) => {
    await page.context().grantPermissions(["clipboard-read", "clipboard-write"]);
    await open(page, "/?mock=1&discord=in#/community");
    const row = post(page, "p2");
    await row.getByRole("button", { name: "More for The launcher window is black at start" }).click();
    await page.getByRole("menuitem", { name: "Copy link" }).click();
    await expect(page.locator(".k-toast").filter({ hasText: "Link copied" })).toBeVisible();
    await row.getByRole("button", { name: /^More for/ }).click();
    await page.getByRole("menuitem", { name: "It's fixed" }).click();
    await expect(page.locator(".k-toast").filter({ hasText: "Marked as solved" })).toContainText("Your post has the Solved tag now. Thanks for saying so.");
    await expect(row).toContainText("Solved");
    // solved already: It's fixed is off
    await row.getByRole("button", { name: /^More for/ }).click();
    await expect(page.getByRole("menuitem", { name: "It's fixed" })).toBeDisabled();
    await page.getByRole("menuitem", { name: "Delete my post" }).click();
    const arch = page.getByRole("dialog", { name: "Delete this post?" });
    await expect(arch).toContainText("The post and its replies are removed from Discord. The Kiln team's copy of your report zip is deleted too.");
    await arch.getByRole("textbox").fill("delete");
    await arch.getByRole("button", { name: "Delete", exact: true }).click();
    await expect(post(page, "p2")).toHaveCount(0);
    expect(await requests(page)).toEqual(expect.arrayContaining(["post-solved", "post-delete"]));
  });

  test("offline: Open in Discord still works, It's fixed and Delete are off", async ({ page }) => {
    await open(page, "/?mock=1&discord=in#/community");
    const row = post(page, "p2");
    await expect(row).toBeVisible();
    await mock(page, (h) => h.community.setOnline(false));
    await expect(page.locator(".k-page-head")).toContainText("You're offline.");
    await expect(row.getByRole("button", { name: "Open in Discord" })).toBeEnabled();
    await row.getByRole("button", { name: /^More for/ }).click();
    await expect(page.getByRole("menuitem", { name: "It's fixed" })).toBeDisabled();
    await expect(page.getByRole("menuitem", { name: "Delete my post" })).toBeDisabled();
  });
});

test.describe("Steam Deck", () => {
  test.use({ viewport: { width: 1280, height: 800 } });

  test("a post: its QR code, It's fixed, Hold to delete my post", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck&discord=in#/deck/community");
    await page.locator('[data-deck-id="post-p2"]').click();
    const d = page.locator('[data-deck-dialog="community-post"]');
    await expect(d).toContainText("Waiting");
    await expect(d).toContainText("Scan to open the post on your phone.");
    await d.getByRole("button", { name: "It's fixed" }).click();
    await expect(page.locator('[data-deck-id="post-p2"]')).toContainText("Solved");
    await expect(page.locator('[data-deck-dialog="community-post"]')).toHaveCount(0);
    await page.locator('[data-deck-id="post-p1"]').click();
    const hold = page.locator('[data-deck-dialog="community-post"] [data-deck-hold]');
    await expect(hold).toContainText("Hold to delete my post");
    // a short press does nothing; holding deletes
    await hold.evaluate((el) => el.dispatchEvent(new CustomEvent("deck-hold", { detail: true })));
    await page.waitForTimeout(300);
    await hold.evaluate((el) => el.dispatchEvent(new CustomEvent("deck-hold", { detail: false })));
    await expect(page.locator('[data-deck-id="post-p1"]')).toHaveCount(1);
    await hold.evaluate((el) => el.dispatchEvent(new CustomEvent("deck-hold", { detail: true })));
    await page.waitForTimeout(1500);
    await expect(page.locator('[data-deck-id="post-p1"]')).toHaveCount(0);
  });
});
