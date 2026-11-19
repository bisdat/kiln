// Stop waiting (29.18; shell/play.ts PlayControl.stopWaiting): Play pressed, Steam took the start and no game came (the
// owner cancelled the launch in Steam's "Processing Vulkan shaders"), and the window waited 5 minutes with no way out.
// While Play waits for a game that does not exist yet, the Play page and the Deck's Home offer "Stop waiting": Play is
// back at once, and the backend's start is dropped too (launch_end), so nothing brings the wait back. Never before Steam
// was asked, never once the game runs. The browser mock's `noProcess` start is a Steam start that never comes.
import { expect, test, type Page } from "@playwright/test";

type Mock = { __kilnMock: { stopGame: () => void; scriptStart: (o: unknown) => void; calls: () => string[]; emit: (n: string, p: unknown) => void;
  planCommand: (cmd: string, plan: unknown) => void; openGate: (name: string) => void } };
const mock = (page: Page) => ({
  script: (o: unknown) => page.evaluate((x) => (window as unknown as Mock).__kilnMock.scriptStart(x), o),
  stop: () => page.evaluate(() => (window as unknown as Mock).__kilnMock.stopGame()),
  calls: (cmd: string) => page.evaluate((c) => (window as unknown as Mock).__kilnMock.calls().filter((x) => x === c).length, cmd),
  // what the backend's watcher sends: the game's status changed, the running game wrote progress.json
  emit: (kind: string) => page.evaluate((k) => (window as unknown as Mock).__kilnMock.emit("kiln://game", { gameId: "client-steam", kind: k }), kind),
});

async function open(page: Page, query = "") {
  await page.goto(`/${query}#/`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

const bar = (page: Page) => page.getByRole("progressbar", { name: "Starting the game" });
const mouth = (page: Page) => page.locator(".k-door");
const stopWaiting = (page: Page) => page.getByRole("button", { name: "Stop waiting" });
const fits = (page: Page) => page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1 && document.documentElement.scrollHeight <= innerHeight + 1);

test("Stop waiting: a Steam start that never comes gives Play back at once, and nothing brings the wait back", async ({ page }) => {
  test.setTimeout(60_000);
  await open(page);
  await mock(page).script({ scale: 0.05, noProcess: true });
  await mouth(page).click();
  await expect(page.locator(".k-start-line")).toHaveText("Starting the game through Steam", { timeout: 15_000 });
  await expect(stopWaiting(page)).toBeVisible();
  expect(await fits(page), "the start with Stop waiting fits").toBe(true);

  await stopWaiting(page).click();
  // at once: the 5 minutes of the wait (STEAM_WAIT_MS) are far off
  await expect(mouth(page)).toHaveAccessibleName(/^Play /, { timeout: 2000 });
  await expect(bar(page)).toHaveCount(0);
  await expect(stopWaiting(page)).toHaveCount(0);
  await expect(page.locator(".k-mods")).toBeVisible();
  await expect(page.locator(".k-head-state")).toHaveText("Stopped");
  expect(await mock(page).calls("launch_end"), "the backend's start is dropped").toBe(1);
  // the backend has no start any more: its next answer brings no wait back
  expect(await page.evaluate(async () => (await import("/src/api/commands.ts")).launchProgress("client-steam"))).toBeNull();

  // nothing restarts the wait: the watcher's events, time; no poll, and Play stays
  await mock(page).emit("status");
  await mock(page).emit("progress");
  await page.waitForTimeout(1500);
  const n = await mock(page).calls("launch_progress");
  await page.waitForTimeout(4000);
  expect(await mock(page).calls("launch_progress") - n, "no poll once the wait is stopped").toBe(0);
  await expect(mouth(page)).toHaveAccessibleName(/^Play /);
  await expect(bar(page)).toHaveCount(0);
  await expect(stopWaiting(page)).toHaveCount(0);

  // and Play starts again as usual
  await mouth(page).click();
  await expect(page.locator(".k-start-line")).toHaveText("Starting the game through Steam", { timeout: 15_000 });
  await stopWaiting(page).click();
  await expect(mouth(page)).toHaveAccessibleName(/^Play /, { timeout: 2000 });
  expect(await mock(page).calls("launch_end")).toBe(2);
});

test("Stop waiting is not offered before Steam was asked, nor once the game runs", async ({ page }) => {
  test.setTimeout(90_000);
  await open(page);
  // Steam takes 300 s (6 s here) to make the game's process; launch_play waits at a gate first
  await mock(page).script({ scale: 0.02, steamMs: 300_000 });
  await page.evaluate(() => (window as unknown as Mock).__kilnMock.planCommand("launch_play", { gate: "steam" }));
  await mouth(page).click();
  await expect(bar(page)).toBeVisible();
  // launch_play has not answered: Steam was not asked yet
  await page.waitForTimeout(1500);
  await expect(stopWaiting(page)).toHaveCount(0);
  await page.evaluate(() => (window as unknown as Mock).__kilnMock.openGate("steam"));
  await expect(stopWaiting(page)).toBeVisible();
  // the game's process comes: the running views as before, without Stop waiting
  await expect(page.locator(".k-head-state")).toHaveText(/^Running/, { timeout: 30_000 });
  await expect(stopWaiting(page)).toHaveCount(0);
  expect(await mock(page).calls("launch_end")).toBe(0);
  await mock(page).stop();
  await expect(page.locator(".k-head-state")).toHaveText("Stopped");
});

test("Stop waiting fits 1024×640, clear of the mouth", async ({ page }) => {
  test.setTimeout(60_000);
  await page.setViewportSize({ width: 1024, height: 640 });
  await open(page);
  await mock(page).script({ scale: 0.05, noProcess: true });
  await mouth(page).click();
  await expect(page.locator(".k-start-line")).toHaveText("Starting the game through Steam", { timeout: 15_000 });
  await expect(stopWaiting(page)).toBeVisible();
  expect(await fits(page), "1024x640").toBe(true);
  const link = await stopWaiting(page).boundingBox();
  const door = await mouth(page).boundingBox();
  expect(link && door && link.x + link.width <= door.x + 1, "clear of the mouth").toBeTruthy();
  await stopWaiting(page).click();
  await expect(mouth(page)).toHaveAccessibleName(/^Play /, { timeout: 2000 });
  await expect(bar(page)).toHaveCount(0);
});

test("the Deck's Home offers Stop waiting too, and keeps a focus after it", async ({ page }) => {
  test.setTimeout(60_000);
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto("/?deck=steamdeck#/");
  await expect(page.locator('[data-deck-screen="home"]')).toBeVisible();
  const focused = () => page.evaluate(() => (document.activeElement as HTMLElement | null)?.dataset?.deckId ?? null);
  await expect.poll(focused).toBe("mouth");
  await mock(page).script({ scale: 0.05, noProcess: true });
  await page.keyboard.press("Enter");
  const deck = page.getByTestId("deck-start");
  await expect(deck).toContainText("Starting the game through Steam", { timeout: 15_000 });
  const stop = page.locator('[data-deck-id="stop-waiting"]');
  await expect(stop).toBeVisible();
  await expect(stop).toHaveText("Stop waiting");
  expect(await fits(page), "deck 1280x800").toBe(true);
  // the mouth keeps the focus through the start (Stop waiting does not take it)
  expect(await focused()).toBe("mouth");
  await stop.focus();
  await page.keyboard.press("Enter");
  await expect(page.locator(".deck-mouth")).toHaveAccessibleName("Play", { timeout: 2000 });
  await expect(deck).toHaveCount(0);
  await expect(stop).toHaveCount(0);
  await expect(page.getByTestId("deck-state")).toHaveText("Stopped");
  expect(await mock(page).calls("launch_end")).toBe(1);
  // a gamepad is never left with nothing focused
  await expect.poll(focused).not.toBeNull();
  await page.waitForTimeout(3000);
  await expect(page.locator(".deck-mouth")).toHaveAccessibleName("Play");
});
