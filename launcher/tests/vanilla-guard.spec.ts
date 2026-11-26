// One start at a time (0.2.0-dev.31.13; src-tauri commands/launch.rs refuse_pending, shell/play.ts seedLaunch): the page
// loaded again after its web process ended (src-tauri/src/webproc.rs) knew nothing of a Vanilla start ("Just once":
// Vanilla) Steam was asked for, and offered Play again: a second press could ask Steam again or start a second Proton.
// The backend follows Vanilla starts too and refuses a second start of either kind while one is pending; the page
// loaded again takes the wait over from launch_progress, as Vanilla (no bar, as before). The browser mock's `noProcess`
// start is a Steam start that never comes; `kiln.mock.persist` keeps the backend's starts over the reload, as the
// launcher's process keeps them.
import { expect, test, type Page } from "@playwright/test";

type Mock = { __kilnMock: { scriptStart: (o: unknown) => void; calls: () => string[]; stopGame: () => void } };
const mock = (page: Page) => ({
  script: (o: unknown) => page.evaluate((x) => (window as unknown as Mock).__kilnMock.scriptStart(x), o),
  calls: (cmd: string) => page.evaluate((c) => (window as unknown as Mock).__kilnMock.calls().filter((x) => x === c).length, cmd),
  stop: () => page.evaluate(() => (window as unknown as Mock).__kilnMock.stopGame()),
});

async function open(page: Page) {
  await page.addInitScript(() => sessionStorage.setItem("kiln.mock.persist", "1"));
  await page.goto("/#/");
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

const bar = (page: Page) => page.getByRole("progressbar", { name: "Starting the game" });
const mouth = (page: Page) => page.locator(".k-door");
const stopWaiting = (page: Page) => page.getByRole("button", { name: "Stop waiting" });

async function playVanilla(page: Page) {
  await page.getByRole("button", { name: "Vanilla", exact: true }).click();
  await page.getByRole("dialog", { name: "Play vanilla once" }).getByRole("button", { name: "Play vanilla" }).click();
}

/** What the backend answers a second start now: launch_begin's and launch_play's error codes and kinds. */
const secondStart = (page: Page, mode: string) => page.evaluate(async (m) => {
  const api = await import("/src/api/commands.ts");
  const out: string[] = [];
  for (const call of [() => api.launchBegin("client-steam", m as never), () => api.launchPlay("client-steam", { mode: m as never, profileId: null })]) {
    try { await call(); out.push("ok"); } catch (e) {
      const k = e as { code: string; message: string; detail?: { kind?: string } };
      out.push(`${k.code} ${k.detail?.kind ?? ""}: ${k.message}`);
    }
  }
  return out;
}, mode);

test("a reload during a Vanilla start's wait for Steam shows the wait, not Play", async ({ page }) => {
  test.setTimeout(90_000);
  await open(page);
  await mock(page).script({ scale: 0.05, noProcess: true });
  await playVanilla(page);
  // Steam was asked: the wait, said as before for Vanilla (no bar), with Stop waiting
  await expect(stopWaiting(page)).toBeVisible({ timeout: 15_000 });
  await expect(bar(page)).toHaveCount(0);
  await expect(mouth(page)).not.toHaveAccessibleName(/^Play /);

  // the page's web process ended: the launcher loads the page again (the backend keeps its starts)
  await page.reload();
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(stopWaiting(page)).toBeVisible({ timeout: 10_000 });
  await expect(mouth(page)).not.toHaveAccessibleName(/^Play /);
  await expect(mouth(page)).toBeDisabled();
  await expect(bar(page)).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Vanilla", exact: true })).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Without mods" })).toHaveCount(0);
  // it stays the wait (Play is not back a few seconds on)
  await page.waitForTimeout(3000);
  await expect(stopWaiting(page)).toBeVisible();
  await expect(mouth(page)).not.toHaveAccessibleName(/^Play /);

  // the backend refuses a second start of either kind meanwhile, and Steam is not asked again
  const plays = await mock(page).calls("launch_play");
  expect(await secondStart(page, "vanilla")).toEqual(Array(2).fill("busy already_starting: The game is already starting."));
  expect(await secondStart(page, "profile")).toEqual(Array(2).fill("busy already_starting: The game is already starting."));
  expect(await mock(page).calls("launch_play") - plays).toBe(2);
  // the refusals left the pending start as it was
  await expect(stopWaiting(page)).toBeVisible();

  // Stop waiting ends it: Play is back, and a new start goes through
  await stopWaiting(page).click();
  await expect(mouth(page)).toHaveAccessibleName(/^Play /, { timeout: 2000 });
  expect(await mock(page).calls("launch_end")).toBe(1);
  await mock(page).script({ scale: 0.02 });
  await playVanilla(page);
  // a Vanilla game: said as before, without a bar
  await expect(page.locator(".k-head-state")).toHaveText(/^Vanilla/, { timeout: 30_000 });
  await expect(bar(page)).toHaveCount(0);
  await mock(page).stop();
  await expect(page.locator(".k-head-state")).toHaveText("Stopped");
});

test("a Play the backend refuses because a start is pending shows that start's wait and ends nothing", async ({ page }) => {
  test.setTimeout(90_000);
  await open(page);
  await mock(page).script({ scale: 0.05, noProcess: true });
  await playVanilla(page);
  await expect(stopWaiting(page)).toBeVisible({ timeout: 15_000 });
  // another page's view of it (one loaded before the backend followed the start): the window forgets its own wait
  await stopWaitingLocally(page);
  await expect(mouth(page)).toHaveAccessibleName(/^Play /, { timeout: 5000 });
  await mouth(page).click();
  await expect(page.locator(".k-toast").filter({ hasText: "The game is already starting." })).toBeVisible({ timeout: 10_000 });
  // the pending start is not this Play's to end: no launch_end, and its wait is shown again
  expect(await mock(page).calls("launch_end")).toBe(0);
  await expect(stopWaiting(page)).toBeVisible({ timeout: 10_000 });
  await expect(mouth(page)).not.toHaveAccessibleName(/^Play /);
  await stopWaiting(page).click();
  await expect(mouth(page)).toHaveAccessibleName(/^Play /, { timeout: 2000 });
});

/** The page's own memo of the start goes (as in a page that never had it), the backend's start stays. */
async function stopWaitingLocally(page: Page) {
  await page.evaluate(async () => {
    const { useLaunchMemo } = await import("/src/shell/play.ts");
    useLaunchMemo.setState({ at: {}, mode: {} });
  });
}
