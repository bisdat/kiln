import { expect, test } from "@playwright/test";
type Mock = { __kilnMock: { scriptStoppedStart: (s: string) => void; externalStart: (id: string, at?: number) => void; calls: () => string[] }; __kilnPlay: (mode: string) => void };
for (const deck of [false, true]) {
  test.describe(deck ? "Deck stopped start" : "Desktop stopped start", () => {
    test.beforeEach(async ({ page }) => {
      await page.setViewportSize(deck ? { width: 1280, height: 800 } : { width: 1024, height: 640 });
      await page.goto(`/${deck ? "?deck=steamdeck" : ""}#${deck ? "/deck" : "/"}`);
      await page.locator(".k-splash").waitFor({ state: "detached" });
    });
    const says: Record<string, string[]> = {
      roster: ["Survivors roster", "The roster holds at most 128 NPCs.", "Switch one of these mods off"],
      "boot-mod": ["A required boot mod could not prepare its content.", "Switch one of these mods off"],
      content: ["Kiln could not build the NPCs and quests of these mods", "kiln.assets refused the built GameKnowledgeResource",
        "Remove or update these mods"],
    };
    for (const kind of ["roster", "boot-mod", "content"]) test(`${kind}: named notice, filter, dismiss and reload`, async ({ page }) => {
      await page.evaluate((s) => (window as unknown as Mock).__kilnMock.scriptStoppedStart(s), kind);
      const play = page.locator(deck ? ".deck-mouth" : ".k-door");
      await expect(play).toBeEnabled();
      await play.click();
      const notice = page.getByRole("region", { name: "Kiln stopped the start", exact: true });
      await expect(notice).toBeVisible({ timeout: 30_000 });
      await expect(notice).toContainText("New Items");
      await expect(notice).toContainText("Big Stacks");
      for (const text of says[kind]) await expect(notice).toContainText(text);
      await notice.getByRole("button", { name: "Review these mods" }).click();
      await expect(page).toHaveURL(/filter=/);
      if (deck) await expect(page.getByRole("list", { name: "Mods", exact: true }).getByRole("button")).toHaveCount(2);
      else await expect(page.getByRole("textbox", { name: "Filter", exact: true })).toHaveValue("ids:new-items,big-stacks");
      await page.goto(`/${deck ? "?deck=steamdeck" : ""}#${deck ? "/deck" : "/"}`);
      await expect(notice).toBeVisible();
      await notice.getByRole("button", { name: "Dismiss", exact: true }).click();
      await expect(notice).toHaveCount(0);
      await page.reload();
      await page.locator(".k-splash").waitFor({ state: "detached" });
      await expect(notice).toHaveCount(0);
    });
    test("a later successful Steam start clears the persisted notice through reload", async ({ page }) => {
      await page.evaluate(() => (window as unknown as Mock).__kilnMock.scriptStoppedStart("roster"));
      await page.locator(deck ? ".deck-mouth" : ".k-door").click();
      const notice = page.getByRole("region", { name: "Kiln stopped the start", exact: true });
      await expect(notice).toBeVisible({ timeout: 30_000 });
      const calls = await page.evaluate(() => (window as unknown as Mock).__kilnMock.calls().filter((c) => c === "launch_play").length);
      await page.evaluate(() => (window as unknown as Mock).__kilnMock.externalStart("client-steam"));
      await expect(notice).toHaveCount(0);
      await expect(page.locator(deck ? ".deck-mouth" : ".k-door")).toHaveAccessibleName(/Running/, { timeout: 15_000 });
      await expect.poll(() => page.evaluate(() => {
        const stored = JSON.parse(localStorage.getItem("kiln-stopped-starts") ?? "{}");
        return stored.state?.notices?.["client-steam"] ?? null;
      })).toBeNull();
      expect(await page.evaluate(() => (window as unknown as Mock).__kilnMock.calls().filter((c) => c === "launch_play").length)).toBe(calls);
      // Reopen during the newer Steam process. Old IPC evidence is deliberately left in the mock.
      await page.addInitScript(() => {
        const timer = setInterval(() => {
          const mock = (window as unknown as Mock).__kilnMock;
          if (mock) { clearInterval(timer); mock.externalStart("client-steam"); }
        }, 10);
      });
      await page.reload();
      await page.locator(".k-splash").waitFor({ state: "detached" });
      await expect(notice).toHaveCount(0);
    });
    test("reopening during a later Steam process retires a notice not dismissed in the old window", async ({ page }) => {
      await page.evaluate(() => (window as unknown as Mock).__kilnMock.scriptStoppedStart("roster"));
      await page.locator(deck ? ".deck-mouth" : ".k-door").click();
      const notice = page.getByRole("region", { name: "Kiln stopped the start", exact: true });
      await expect(notice).toBeVisible({ timeout: 30_000 });
      await page.addInitScript(() => {
        const timer = setInterval(() => {
          const mock = (window as unknown as Mock).__kilnMock;
          if (mock) { clearInterval(timer); mock.externalStart("client-steam"); }
        }, 10);
      });
      await page.reload();
      await page.locator(".k-splash").waitFor({ state: "detached" });
      await expect(notice).toHaveCount(0);
      await expect.poll(() => page.evaluate(() => JSON.parse(localStorage.getItem("kiln-stopped-starts")!).state.notices["client-steam"] ?? null)).toBeNull();
    });
    test("the refused process keeps its notice while its message box is open", async ({ page }) => {
      await page.evaluate(() => (window as unknown as Mock).__kilnMock.scriptStoppedStart("roster"));
      await page.locator(deck ? ".deck-mouth" : ".k-door").click();
      const notice = page.getByRole("region", { name: "Kiln stopped the start", exact: true });
      await expect(notice).toBeVisible({ timeout: 30_000 });
      await page.evaluate(() => {
        const saved = JSON.parse(localStorage.getItem("kiln-stopped-starts")!).state.notices["client-steam"];
        (window as unknown as Mock).__kilnMock.externalStart("client-steam", Date.parse(saved.utc) - 1000);
      });
      // The old stopped render says Play. This label proves the running status reached the UI.
      await expect(page.locator(deck ? ".deck-mouth" : ".k-door")).toHaveAccessibleName(/Kiln stopped the start/);
      await expect(notice).toBeVisible();
    });
    for (const kind of ["unknown", "garbled", "stale", "quit"]) test(`${kind}: no stop notice at the end`, async ({ page }) => {
      await page.evaluate((s) => (window as unknown as Mock).__kilnMock.scriptStoppedStart(s), kind);
      await page.locator(deck ? ".deck-mouth" : ".k-door").click();
      // Observe a durable completion counter, so this assertion cannot pass before the scripted exit happened.
      await expect.poll(() => page.evaluate(() => (window as unknown as { __kilnStopCompleted?: number }).__kilnStopCompleted ?? 0)).toBe(1);
      await expect(page.getByRole("region", { name: "Kiln stopped the start", exact: true })).toHaveCount(0);
    });
  });
}
