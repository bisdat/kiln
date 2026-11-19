// The tester programme in mock mode (src/community/Testers.tsx; WIREFRAMES D15, K12): the hardware shown before
// anything is sent, Volunteer, the checklist with this hardware's steps, a result per step, Send results, the read
// mark, Stop testing; desktop and Deck.
import { expect, test, type Page } from "@playwright/test";

type Hooks = { community: { requests: () => string[]; readResults: () => void; setOnline: (on: boolean) => void } };
const mock = <T,>(page: Page, f: (h: Hooks) => T) => page.evaluate((src) => (new Function("h", `return (${src})(h)`))((window as unknown as { __kilnMock: Hooks }).__kilnMock), f.toString()) as Promise<T>;
const requests = (page: Page) => mock(page, (h) => h.community.requests());

async function open(page: Page, url: string) {
  await page.goto(url);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

test.describe("desktop", () => {
  test("the pitch says what this PC can test; Volunteer shows the hardware first and sends nothing signed out", async ({ page }) => {
    await open(page, "/?mock=1#/community");
    const card = page.getByTestId("kc-tester");
    await expect(card).toContainText("Kiln's next graphics work needs hardware the team doesn't have");
    await expect(card).toContainText("This PC can test FSR 4.");
    await page.getByTestId("kc-volunteer-open").click();
    const d = page.getByTestId("kc-volunteer-dialog");
    await expect(d).toContainText("What the launcher shares about this PC");
    for (const line of ["AMD Radeon RX 7800 XT", "amdgpu (Mesa RADV)", "16 GB", "Arch Linux", "GE-Proton10-3", "FSR 4"]) await expect(d).toContainText(line);
    await expect(d).toContainText("Sent only when you press Volunteer, to the Kiln team, so they know what you can test.");
    await expect(d).toContainText("Volunteering needs a Discord sign-in: the Tester role is on the Kiln Discord.");
    await expect(page.getByTestId("kc-volunteer")).toHaveText("Sign in with Discord");
    expect((await requests(page)).filter((r) => r.startsWith("tester"))).toEqual([]);
  });

  test("volunteer, try each step, send results, then the team's read mark", async ({ page }) => {
    await open(page, "/?mock=1&discord=in#/community");
    await page.getByTestId("kc-volunteer-open").click();
    await page.getByTestId("kc-volunteer").click();
    await expect(page.locator(".k-toast").filter({ hasText: "You're a tester" })).toBeVisible();
    expect(await requests(page)).toContain("tester-join");
    const list = page.getByTestId("kc-checklist");
    await expect(list).toContainText("Test 0.2.0-dev.30 Beta 2");
    await expect(list).toContainText(/0 of 4 steps tried\. Open until/);
    await expect(list).toContainText("2 steps are for other hardware.");
    await expect(list).toContainText("Turn on FSR 4 · For AMD RX 6000 and newer");
    await expect(page.getByTestId("kc-send-results")).toBeDisabled();
    await expect(list).toContainText("Try each step, or mark it Couldn't try.");
    // the first step is open; Worked
    const step = (id: string) => list.locator(`[data-step="${id}"]`);
    await step("start").getByRole("radio", { name: "Worked" }).click();
    await expect(step("start")).toContainText("Worked");
    await step("fsr4").locator(".kc-step-head").click();
    await step("fsr4").getByRole("radio", { name: "Didn't work" }).click();
    await step("fsr4").getByRole("textbox").fill("Water smears when I turn the camera");
    await step("fsr4").getByRole("textbox").blur();
    for (const id of ["overlay", "reload"]) {
      await step(id).locator(".kc-step-head").click();
      await step(id).getByRole("radio", { name: id === "reload" ? "Couldn't try" : "Worked" }).click();
    }
    // pressed again: back to Not tried, and on again
    await step("reload").getByRole("radio", { name: "Couldn't try" }).click();
    await expect(step("reload")).toContainText("Not tried");
    await step("reload").getByRole("radio", { name: "Couldn't try" }).click();
    await expect(step("reload").locator(".kc-step-head")).toContainText("Couldn't try");
    await expect(page.getByTestId("kc-send-results")).toBeEnabled();
    // the report attached for the team is shown before Send results: its size and files, prepared once and sent as shown
    const attached = page.getByTestId("kc-attached");
    await expect(attached).toContainText("10 files: Kiln's logs and state files, the profile, the launcher's log and lists of the game folder.");
    await attached.getByRole("button", { name: "Show the files" }).click();
    await expect(page.getByTestId("kc-attached-files")).toContainText("game/kiln/logs/kiln.log");
    await expect(attached).toContainText("Saves and mod settings are never in it.");
    await page.getByTestId("kc-send-results").click();
    await expect(page.locator(".k-toast").filter({ hasText: "Results sent" })).toContainText("They're in the build's post in #testing. The team marks them when they've read them.");
    await expect(page.getByTestId("kc-results-state")).toContainText("Results sent just now. The Kiln team hasn't read them yet.");
    expect(await requests(page)).toContain("tester-results+zip");
    await mock(page, (h) => h.community.readResults());
    await expect(page.getByTestId("kc-results-state")).toContainText("Rowan of the Kiln team read your results just now.");
  });

  test("read by the team and replied; Stop testing", async ({ page }) => {
    await open(page, "/?mock=1&discord=read#/community");
    await expect(page.getByTestId("kc-results-state")).toContainText("Rowan of the Kiln team read your results 2 hours ago. They also replied in the build's post on Discord.");
    await page.getByTestId("kc-checklist").getByRole("button", { name: "More for this test build" }).click();
    await page.getByRole("menuitem", { name: "Stop testing" }).click();
    await expect(page.locator(".k-toast").filter({ hasText: "You're not a tester any more" })).toBeVisible();
    await expect(page.getByTestId("kc-tester")).toContainText("Volunteer as a tester");
    expect(await requests(page)).toContain("tester-leave");
  });

  test("a tester with no test build open can Stop testing too: the role and the tester state go", async ({ page }) => {
    await open(page, "/?mock=1&discord=in,nobuild#/community");
    await page.getByTestId("kc-volunteer-open").click();
    await page.getByTestId("kc-volunteer").click();
    await expect(page.locator(".k-toast").filter({ hasText: "You're a tester" })).toBeVisible();
    const card = page.getByTestId("kc-tester");
    await expect(card).toContainText("You're a tester.");
    await expect(card).toContainText("No test build right now. The next one shows here.");
    await expect(page.getByTestId("kc-checklist")).toHaveCount(0);
    await expect(page.getByTestId("kc-account")).toContainText("Tester");
    // it must reach frostdev.io: not while offline
    const leave = card.getByRole("button", { name: "Stop testing" });
    await mock(page, (h) => h.community.setOnline(false));
    await expect(leave).toBeDisabled();
    await mock(page, (h) => h.community.setOnline(true));
    await leave.click();
    await expect(page.locator(".k-toast").filter({ hasText: "You're not a tester any more" })).toBeVisible();
    await expect(card).toContainText("Volunteer as a tester");
    await expect(card).not.toContainText("You're a tester.");
    await expect(page.getByTestId("kc-account")).not.toContainText("Tester");
    expect((await requests(page)).filter((r) => r.startsWith("tester-"))).toEqual(["tester-join", "tester-leave"]);
  });
});

test.describe("Steam Deck", () => {
  test.use({ viewport: { width: 1280, height: 800 } });

  test("volunteer with this Deck's hardware, then a step's result with a pick for what happened", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck&discord=in#/deck/community");
    await page.locator('[data-deck-id="tester"]').click();
    const v = page.getByTestId("deck-volunteer");
    await expect(v).toContainText("What the launcher shares about this Deck");
    await expect(v).toContainText("AMD Custom GPU 0405 (Steam Deck LCD)");
    await expect(v).toContainText("Steam Deck");
    await page.locator('[data-deck-dialog="tester-volunteer"]').getByRole("button", { name: "Volunteer" }).click();
    await expect(page.locator(".deck-toast").filter({ hasText: "You're a tester" })).toBeVisible();
    await page.keyboard.press("Escape");
    await page.locator('[data-deck-id="tester"]').click();
    const plan = page.getByTestId("deck-plan");
    await expect(plan).toContainText("1. Start the game with Kiln");
    await expect(plan).toContainText("For Steam Deck");
    await plan.locator('[data-deck-id="step-deck-battery"]').click();
    await page.getByTestId("deck-step").getByRole("button", { name: "Didn't work" }).click();
    await page.getByTestId("deck-why").locator('[data-deck-id="why-slow"]').click();
    await expect(plan.locator('[data-deck-id="step-deck-battery"]')).toContainText("It was slow or stuttered");
    await expect(plan.locator('[data-deck-id="step-deck-battery"]')).toContainText("Didn't work");
    // the report attached for the team: its size and files before Send results
    await expect(plan.locator('[data-deck-id="attached"]')).toContainText("10 files");
    await plan.locator('[data-deck-id="attached"]').click();
    await expect(page.getByTestId("deck-attached-files")).toContainText("game/kiln/logs/kiln.log");
  });

  test("results read by the team", async ({ page }) => {
    await page.goto("/?mock=1&deck=steamdeck&discord=read#/deck/community");
    await page.locator('[data-deck-id="tester"]').click();
    await expect(page.getByTestId("deck-results-state")).toContainText("Rowan of the Kiln team read your results 2 hours ago.");
  });
});
