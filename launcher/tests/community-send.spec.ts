// "Send to Kiln support" in mock mode (src/community/SendDialog.tsx, DeckSend.tsx; WIREFRAMES D6 to D10, K5 to K9):
// every entry point (Logs, Play's banners, the crash notice, the Community page, Deck Home, the Deck's tab), the draft
// kept through a sign-in round trip and every error, who sees the zip, and the known problem shown first.
import { expect, test, type Page } from "@playwright/test";

type Hooks = {
  community: { requests: () => string[]; approve: () => void; setOnline: (on: boolean) => void; setRelay: (up: boolean) => void; join: () => void };
  opened: () => string[]; setNoProxy: (on: boolean) => void; crash: () => void; externalStart: (id: string, at?: number) => void;
};
const mock = <T,>(page: Page, f: (h: Hooks) => T) => page.evaluate((src) => (new Function("h", `return (${src})(h)`))((window as unknown as { __kilnMock: Hooks }).__kilnMock), f.toString()) as Promise<T>;
const requests = (page: Page) => mock(page, (h) => h.community.requests());
const opened = (page: Page) => mock(page, (h) => h.opened());

async function open(page: Page, url: string) {
  await page.goto(url);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}
const dialog = (page: Page, name: string) => page.getByRole("dialog", { name });

async function writeAndCheck(page: Page, line = "The game crashes when I open the map") {
  await expect(page.getByTestId("kc-write")).toBeVisible();
  await page.locator("#kc-line").fill(line);
  await page.getByTestId("kc-next").click();
  await expect(page.getByTestId("kc-check")).toBeVisible();
}

test.describe("desktop", () => {
  test("from the Logs page: write, check what is sent, sent", async ({ page }) => {
    await open(page, "/?mock=1&discord=in#/logs");
    await page.getByTestId("support-report").getByRole("button", { name: "Send to Kiln support" }).click();
    const write = page.getByTestId("kc-write");
    await expect(write).toBeVisible();
    // the line is needed
    await page.getByTestId("kc-next").click();
    await expect(write).toContainText("Write one line first.");
    await expect(page.locator("#kc-line")).toHaveAttribute("placeholder", "Kiln doesn't load since the last game update");
    await write.getByRole("radio", { name: "The game crashes" }).click();
    await expect(page.locator("#kc-line")).toHaveAttribute("placeholder", "The game crashes when I open the map");
    await page.locator("#kc-line").fill("The game crashes when I open the map");
    await expect(write).toContainText("36 of 90");
    await page.locator("#kc-more").fill("It worked yesterday.");
    await page.getByTestId("kc-next").click();
    const post = page.getByTestId("kc-postmock");
    await expect(post).toContainText("The game crashes when I open the map");
    await expect(post.locator(".kc-postmock-tags")).toHaveText("CrashLinux");
    await expect(post).toContainText("It worked yesterday.");
    await expect(post).toContainText("Night Sky 0.3.1 did not load: incompatible with Weather Plus 1.4.2.");
    await expect(post).toContainText(/Kiln 0\.2\.0-dev\.29\.18 · Launcher 0\.2\.0-dev\.29\.18 · Arch Linux · GE-Proton10-3 · game build 26404966 · \d+ mods? on/);
    await expect(page.getByTestId("kc-check")).toContainText("Posting as emberfox. Posted by the Kiln bot with you mentioned, so Discord tells you about replies.");
    await page.getByTestId("kc-files-toggle").click();
    await expect(page.getByTestId("kc-files")).toContainText("game/kiln/logs/kiln.log");
    await expect(page.locator('input[name="kc-zip"][value="team"]')).toBeChecked();
    await expect(page.getByTestId("kc-check")).toContainText("In every file your user name, PC name, Steam names, Steam ID and IP addresses are replaced.");
    await page.getByTestId("kc-send-now").click();
    const sent = page.getByTestId("kc-sent");
    await expect(sent).toContainText("Your post is up in #support: “The game crashes when I open the map”.");
    await expect(sent).toContainText("The report zip went to the Kiln team only.");
    await dialog(page, "Sent").getByRole("button", { name: "Open in Discord" }).click();
    await expect.poll(() => opened(page)).toContain("https://discord.com/channels/1568454645683195976/15731908842100009");
    expect(await requests(page)).toContain("reports");
    await page.goto("/?mock=1&discord=in#/community");
    await expect(page.getByTestId("kc-post").first()).toContainText("The game crashes when I open the map");
  });

  test("signed out: write first, sign in from the check step, the draft comes back", async ({ page }) => {
    await open(page, "/?mock=1#/community");
    await page.getByTestId("kc-send").click();
    await writeAndCheck(page, "Mods don't load since the game update");
    await expect(page.getByTestId("kc-send-state")).toContainText("Sending needs a Discord sign-in. Your draft stays here while you sign in.");
    // nothing reached the relay while writing and checking
    expect((await requests(page)).filter((r) => r === "reports")).toEqual([]);
    await page.getByTestId("kc-send-signin").click();
    await page.getByTestId("kc-signin-open").click();
    await expect(page.getByTestId("kc-check")).toBeVisible({ timeout: 10_000 });
    await expect(page.getByTestId("kc-postmock")).toContainText("Mods don't load since the game update");
    await expect(page.getByTestId("kc-check")).toContainText("Posting as emberfox.");
    await page.getByTestId("kc-send-now").click();
    await expect(page.getByTestId("kc-sent")).toBeVisible();
  });

  test("Cancel keeps a draft with words: the next Send to Kiln support opens it again", async ({ page }) => {
    await open(page, "/?mock=1&discord=in#/community");
    await page.getByTestId("kc-send").click();
    await page.locator("#kc-line").fill("The launcher shows a black window");
    await dialog(page, "Send to Kiln support").getByRole("button", { name: "Cancel" }).click();
    await page.getByTestId("kc-send").click();
    await expect(page.locator("#kc-line")).toHaveValue("The launcher shows a black window");
  });

  test("frostdev.io down, then offline: the draft stays, Retry, Save report", async ({ page }) => {
    await open(page, "/?mock=1&discord=in#/community");
    await page.getByTestId("kc-send").click();
    await writeAndCheck(page);
    await mock(page, (h) => h.community.setRelay(false));
    await page.getByTestId("kc-send-now").click();
    const state = page.getByTestId("kc-send-state");
    await expect(state).toContainText("Kiln support can't be reached");
    await expect(state).toContainText("frostdev.io isn't answering. Your draft is kept. Try again in a few minutes.");
    await mock(page, (h) => h.community.setRelay(true));
    await mock(page, (h) => h.community.setOnline(false));
    await state.getByRole("button", { name: "Retry" }).click();
    await expect(state).toContainText("You're offline");
    await expect(state).toContainText("Your draft is kept. Send it when you're back online, or save the report and post it yourself.");
    await expect(page.getByTestId("kc-send-now")).toBeDisabled();
    await expect(page.getByTestId("kc-postmock")).toContainText("The game crashes when I open the map");
    await state.getByRole("button", { name: "Save report" }).click();
    await expect(page).toHaveURL(/#\/logs$/);
  });

  test("not in the server: Join, then I've joined", async ({ page }) => {
    await open(page, "/?mock=1&discord=notmember#/community");
    await page.getByTestId("kc-send").click();
    await writeAndCheck(page);
    await page.getByTestId("kc-send-now").click();
    const state = page.getByTestId("kc-send-state");
    await expect(state).toContainText("You're not in the Kiln Discord yet");
    await state.getByRole("button", { name: "Join the Kiln Discord" }).click();
    await expect.poll(() => opened(page)).toContain("https://discord.gg/7pjr8SMTfU");
    await state.getByRole("button", { name: "I've joined" }).click();
    await expect(page.locator(".k-toast").filter({ hasText: "You're still not in the Kiln Discord." })).toBeVisible();
    await mock(page, (h) => h.community.join());
    await state.getByRole("button", { name: "I've joined" }).click();
    await expect(state).not.toContainText("You're not in the Kiln Discord yet");
    await page.getByTestId("kc-send-now").click();
    await expect(page.getByTestId("kc-sent")).toBeVisible();
  });

  test("too many reports: how many, when again, the last post", async ({ page }) => {
    await open(page, "/?mock=1&discord=in,ratelimit#/community");
    await page.getByTestId("kc-send").click();
    await writeAndCheck(page);
    await page.getByTestId("kc-send-now").click();
    const state = page.getByTestId("kc-send-state");
    await expect(state).toContainText("That's a lot of reports");
    await expect(state).toContainText("You sent 3 reports in the last hour. Wait for a reply, or add to your last post on Discord. You can send again in 38 minutes.");
    await state.getByRole("button", { name: "Open my last post" }).click();
    await expect.poll(() => opened(page)).toContain("https://discord.com/channels/1568454645683195976/15731308842100003");
  });

  test("too big: the size and the limit, Send without the zip", async ({ page }) => {
    await open(page, "/?mock=1&discord=in,toobig#/community");
    await page.getByTestId("kc-send").click();
    await writeAndCheck(page);
    await page.getByTestId("kc-send-now").click();
    const state = page.getByTestId("kc-send-state");
    await expect(state).toContainText("The report is too big to send");
    await expect(state).toContainText(/This report is 2\d(\.\d)? MB, and up to 8(\.\d)? MB can be sent\./);
    await state.getByRole("button", { name: "Send without the zip" }).click();
    await expect(page.getByTestId("kc-sent")).toContainText("The post went without the zip.");
  });

  test("a known problem shows its fix first, and sending stays possible", async ({ page }) => {
    await open(page, "/?mock=1&discord=in,known#/community");
    await page.getByTestId("kc-send").click();
    const known = page.getByTestId("kc-known");
    await expect(known).toContainText("Known problem");
    await expect(known).toContainText("Night Sky and Weather Plus crash together");
    await expect(known).toContainText("If this fixes it, there's nothing to send. If it doesn't, carry on.");
    await expect(known.getByRole("button", { name: "Show my mods" })).toBeVisible();
    await writeAndCheck(page);
  });

  test("Play's Kiln is not loading banner: the fix first, then Send to Kiln support with the launch option's known problem", async ({ page }) => {
    await open(page, "/?mock=1&discord=in#/");
    await mock(page, (h) => h.setNoProxy(true));
    await page.getByRole("button", { name: "Without mods" }).click();
    await expect(page.locator(".k-head-state")).toHaveText(/^No Kiln/, { timeout: 15_000 });
    await expect(page.getByText("Kiln is not loading")).toBeVisible({ timeout: 15_000 });
    const banner = page.locator(".k-banner", { hasText: "Kiln is not loading" });
    const buttons = await banner.getByRole("button").allInnerTexts();
    expect(buttons.indexOf("Send to Kiln support")).toBeGreaterThan(buttons.indexOf("Copy"));
    await banner.getByRole("button", { name: "Send to Kiln support" }).click();
    await expect(page.getByTestId("kc-write").getByRole("radio", { name: "Kiln doesn't load" })).toHaveAttribute("aria-checked", "true");
    await expect(page.getByTestId("kc-known")).toContainText("Steam's launch option is missing");
    await mock(page, (h) => h.setNoProxy(false));
  });

  test("the crash notice on Play: the crash report first, then Send to Kiln support (the crash kind)", async ({ page }) => {
    await open(page, "/?mock=1&discord=in&problem=crash#/");
    const notice = page.locator(".k-banner", { hasText: "The game closed unexpectedly" });
    await expect(notice).toBeVisible({ timeout: 10_000 });
    await expect(page.getByTestId("crash-notice")).toHaveText(/^It closed at .+, 4 minutes after it started\. Kiln wrote a crash report\.$/);
    const buttons = await notice.getByRole("button").allInnerTexts();
    expect(buttons.slice(0, 2)).toEqual(["See the crash report", "Send to Kiln support"]);
    await notice.getByRole("button", { name: "Send to Kiln support" }).click();
    await expect(page.getByTestId("kc-write").getByRole("radio", { name: "The game crashes" })).toHaveAttribute("aria-checked", "true");
  });

  test("hidden in Settings: no Send to Kiln support anywhere; the crash notice still shows", async ({ page }) => {
    await open(page, "/?mock=1&discord=hidden&problem=crash#/");
    await expect(page.locator(".k-banner", { hasText: "The game closed unexpectedly" })).toBeVisible({ timeout: 10_000 });
    await expect(page.getByRole("button", { name: "Send to Kiln support" })).toHaveCount(0);
    await page.goto("/?mock=1&discord=hidden#/logs");
    await expect(page.getByTestId("support-report")).toBeVisible();
    await expect(page.getByRole("button", { name: "Send to Kiln support" })).toHaveCount(0);
    expect(await requests(page)).toEqual([]);
  });
});

test.describe("Steam Deck", () => {
  test.use({ viewport: { width: 1280, height: 800 } });
  const focused = (page: Page) => page.evaluate(() => (document.activeElement as HTMLElement | null)?.dataset?.deckId ?? null);

  test("pick what and when, check, sign in by phone with the draft kept, send, the post as a QR code", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck#/deck/community");
    await page.locator('[data-deck-id="send"]').click();
    await expect(page.getByTestId("deck-send-what")).toBeVisible();
    await expect.poll(() => focused(page)).toBe("kind-noload");
    await page.locator('[data-deck-id="kind-crash"]').click();
    await expect(page.getByTestId("deck-send-when")).toContainText("The game crashes when I load a world");
    await page.locator('[data-deck-id="when-load"]').click();
    const check = page.getByTestId("deck-send-check");
    await expect(check.locator('[data-deck-id="title"]')).toContainText("The game crashes when I load a world");
    // the zip's audience: team, everyone, nobody
    const zip = check.locator('[data-deck-id="zip"]');
    await expect(zip).toContainText("The Kiln team only");
    await zip.click(); await expect(zip).toContainText("Everyone in #support");
    await zip.click(); await expect(zip).toContainText("Nobody");
    await zip.click(); await expect(zip).toContainText("The Kiln team only");
    // words are optional; Type opens the field
    await check.locator('[data-deck-id="words"]').click();
    await page.getByTestId("deck-type").fill("Only on my big base world");
    await page.locator('[data-deck-dialog="send-type"]').getByRole("button", { name: "Done" }).click();
    await expect(check.locator('[data-deck-id="words"]')).toContainText("Only on my big base world");
    await page.locator('[data-deck-dialog="send-check"]').getByRole("button", { name: "Sign in with Discord to send" }).click();
    await expect(page.getByTestId("deck-qr")).toBeVisible();
    await mock(page, (h) => h.community.approve());
    await expect(check).toBeVisible({ timeout: 10_000 });
    await expect(check.locator('[data-deck-id="words"]')).toContainText("Only on my big base world");
    await page.locator('[data-deck-dialog="send-check"]').getByRole("button", { name: "Send to Kiln support" }).click();
    const sent = page.getByTestId("deck-sent");
    await expect(sent).toContainText("Your post is up in #support: “The game crashes when I load a world”.");
    await expect(sent.locator("svg")).toHaveAttribute("data-qr", /^https:\/\/discord\.com\/channels\//);
  });

  test("Deck Home after a crash: the line and Send to Kiln support", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck&discord=in&problem=crash#/deck");
    await expect(page.getByTestId("deck-crash")).toContainText("The game closed unexpectedly", { timeout: 10_000 });
    await page.locator('[data-deck-id="send-support"]').click();
    await expect.poll(() => focused(page)).toBe("kind-crash");
  });

  test("the known problem on the check step, what is sent, and offline", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck&discord=in,known#/deck/community");
    await page.locator('[data-deck-id="send"]').click();
    await page.locator('[data-deck-id="kind-mod"]').click();
    await page.locator('[data-deck-id="when-play"]').click();
    const check = page.getByTestId("deck-send-check");
    await check.locator('[data-deck-id="known"]').click();
    await expect(page.getByTestId("deck-known")).toContainText("Night Sky 0.3 crashes the game when Weather Plus 1.4 changes the sky.");
    await page.locator('[data-deck-dialog="send-known"]').getByRole("button", { name: "OK" }).click();
    await check.locator('[data-deck-id="files"]').click();
    await expect(page.getByTestId("deck-what-is-sent")).toContainText("game/kiln/logs/kiln.log");
    await page.locator('[data-deck-dialog="send-files"]').getByRole("button", { name: "OK" }).click();
    await mock(page, (h) => h.community.setOnline(false));
    await expect(page.getByTestId("deck-send-offline")).toContainText("You're offline");
    await expect(page.locator('[data-deck-dialog="send-check"]').getByRole("button", { name: "Send to Kiln support" })).toBeDisabled();
  });
});
