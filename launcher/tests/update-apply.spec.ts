// The launcher update in one click (browser mock; shell/UpdateNotice.tsx, shell/updateApply.ts): Update, then the
// launcher restarts by itself after a short countdown (Later stops it), unless the game runs or Pack Studio or an editor
// has unsaved work (it says why and the player restarts); after the restart the Kiln runtime it brought goes into the
// closed game by itself (the ordinary install, a toast). And the "Updated to" notice from the first status on, also
// before the game's status has loaded (a player's report on 30.3: the notice had no Update game until a poll the
// unfocused window never sent).
import { expect, test, type Page } from "@playwright/test";

const BUNDLE = "0.2.0-dev+20261104.0f1ffa6"; // a runtime bundle the mock has

async function open(page: Page, query = "", delayMs = 1500) {
  await page.addInitScript((ms) => { (window as unknown as { __kilnRestartDelayMs: number }).__kilnRestartDelayMs = ms; }, delayMs);
  await page.goto(`/${query}#/`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

const mock = (page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(([f, a]) => (window as unknown as { __kilnUpdateMock: Record<string, (...x: unknown[]) => unknown> }).__kilnUpdateMock[f as string](...(a as unknown[])), [fn, args] as const);
const game = (page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(([f, a]) => (window as unknown as { __kilnMock: Record<string, (...x: unknown[]) => unknown> }).__kilnMock[f as string](...(a as unknown[])), [fn, args] as const);
const calls = (page: Page) => mock(page, "calls") as Promise<string[]>;
const restarts = async (page: Page) => (await calls(page)).filter((c) => c === "update_restart").length;
/** Marks unsaved work the way Pack Studio's drafts and the editors do (shell/unsaved.ts), or clears it. */
const unsaved = (page: Page, kind: "studio" | "editor" | null) =>
  page.evaluate(async (k) => { (await import("/src/shell/unsaved.ts")).setUnsaved("test", k); }, kind);
const toastOk = (page: Page) => page.locator(".k-toast", { hasText: `Kiln ${BUNDLE} installed` });

async function updateNow(page: Page) {
  await mock(page, "offer", "0.2.0-dev.43");
  await page.getByRole("dialog", { name: "Kiln 0.2.0-dev.43" }).getByRole("button", { name: "Update", exact: true }).click();
  const ready = page.getByRole("dialog", { name: "Ready to restart" });
  await expect(ready).toBeVisible({ timeout: 8000 });
  return ready;
}

test("one click: Update, the launcher restarts by itself, and its Kiln goes into the closed game", async ({ page }) => {
  await open(page);
  await game(page, "setUpgradeAvailable", BUNDLE);
  const ready = await updateNow(page);
  await expect(ready.getByTestId("update-restart-in")).toHaveText(/^Restarting by itself in [12] s$/);
  await expect(page.getByRole("dialog", { name: "Updated to 0.2.0-dev.43" })).toBeVisible({ timeout: 6000 });
  await expect(toastOk(page)).toContainText("It came with the launcher update to 0.2.0-dev.43.", { timeout: 15_000 });
  await expect(page.locator(".k-upd")).toHaveCount(0);
  expect(await game(page, "installs")).toEqual([BUNDLE]);
  expect((await calls(page)).filter((c) => ["update_install", "update_restart", "update_ack"].includes(c))).toEqual(["update_install", "update_restart", "update_ack"]);
});

test("Later stops the restart by itself; Restart still restarts", async ({ page }) => {
  await open(page);
  const ready = await updateNow(page);
  await expect(ready.getByTestId("update-restart-in")).toBeVisible();
  await ready.getByRole("button", { name: "Later" }).click();
  await page.waitForTimeout(2500);
  expect(await restarts(page)).toBe(0);
  await page.getByRole("button", { name: "Launcher update: Restart", exact: true }).click();
  await expect(ready.getByTestId("update-restart-in")).toHaveCount(0);
  await ready.getByRole("button", { name: "Restart", exact: true }).click();
  await expect(page.getByRole("dialog", { name: "Updated to 0.2.0-dev.43" })).toBeVisible();
});

test("while the game runs the launcher does not restart by itself and says why", async ({ page }) => {
  await open(page);
  await page.locator(".k-door").click();
  await expect(page.locator(".k-head-state")).toHaveText(/^Running/, { timeout: 8000 });
  const ready = await updateNow(page);
  await expect(ready.getByTestId("update-restart-why")).toHaveText("The game is running, so the launcher waits. Restart when you are done playing.");
  await expect(ready.getByTestId("update-restart-in")).toHaveCount(0);
  await expect(ready.getByRole("button", { name: "Restart", exact: true })).toBeEnabled();
  await page.waitForTimeout(2500);
  expect(await restarts(page)).toBe(0);
  // held for good: the game closing does not start it either (today's buttons; the player restarts)
  await game(page, "stopGame");
  await expect(ready.getByTestId("update-restart-why")).toHaveCount(0, { timeout: 6000 });
  await page.waitForTimeout(2500);
  expect(await restarts(page)).toBe(0);
  await ready.getByRole("button", { name: "Restart", exact: true }).click();
  await expect(page.getByRole("dialog", { name: "Updated to 0.2.0-dev.43" })).toBeVisible();
});

test("unsaved work in Pack Studio or an editor holds the restart, and the notice says whose", async ({ page }) => {
  await open(page);
  await unsaved(page, "studio");
  const ready = await updateNow(page);
  const why = ready.getByTestId("update-restart-why");
  await expect(why).toHaveText("Pack Studio has unsaved changes, so the launcher waits. Save them, then restart.");
  await unsaved(page, "editor");
  await expect(why).toHaveText("An editor has unsaved changes, so the launcher waits. Save them, then restart.");
  // the save editor's own store counts too
  await unsaved(page, null);
  await page.evaluate(async () => { (await import("/src/editor/kiln/pending.ts")).usePendingEdits.setState({ dirty: true, name: "Ember" }); });
  await expect(why).toHaveText("An editor has unsaved changes, so the launcher waits. Save them, then restart.");
  await page.waitForTimeout(2500);
  expect(await restarts(page)).toBe(0);
  await expect(ready.getByTestId("update-restart-in")).toHaveCount(0);
});

test("a Pack Studio draft is unsaved work", async ({ page }) => {
  await open(page);
  await page.evaluate(async () => {
    (await import("/src/studio/data/store.ts")).useDrafts.getState().set("pack:raw:test.json", { value: "{}", base: undefined, dirty: true });
  });
  const ready = await updateNow(page);
  await expect(ready.getByTestId("update-restart-why")).toHaveText("Pack Studio has unsaved changes, so the launcher waits. Save them, then restart.");
});

test("the Updated card shows before the game's status has loaded, then its Kiln goes in by itself", async ({ page }) => {
  // game_status waits until the test opens its gate: the notice is there before the game's status, as after a restart
  await page.addInitScript(() => {
    (window as unknown as { __kilnMockPlans: Record<string, unknown[]> }).__kilnMockPlans = { game_status: Array.from({ length: 40 }, () => ({ gate: "status" })) };
  });
  await open(page, `?justUpdated=0.2.0-dev.41&upgrade=${encodeURIComponent(BUNDLE)}`);
  const updated = page.getByRole("dialog", { name: "Updated to 0.2.0-dev.42" });
  await expect(updated).toBeVisible();
  await expect(updated.getByTestId("update-game-loading")).toHaveText("Looking at the game's Kiln…");
  await expect(page.getByRole("button", { name: "Launcher update: Updated", exact: true })).toBeVisible();
  expect(await game(page, "installs")).toEqual([]);
  await game(page, "openGate", "status");
  await expect(toastOk(page)).toContainText("It came with the launcher update to 0.2.0-dev.42.", { timeout: 15_000 });
  await expect(updated).toHaveCount(0);
  expect(await game(page, "installs")).toEqual([BUNDLE]);
  expect(await calls(page)).toContain("update_ack");
});

test("just updated with a newer Kiln while the game runs: the card holds Update game and says why, then installs when the game closes", async ({ page }) => {
  await page.addInitScript(() => {
    (window as unknown as { __kilnMockPlans: Record<string, unknown[]> }).__kilnMockPlans = { game_status: Array.from({ length: 40 }, () => ({ gate: "status" })) };
  });
  await open(page, `?justUpdated=0.2.0-dev.41&upgrade=${encodeURIComponent(BUNDLE)}`);
  const updated = page.getByRole("dialog", { name: "Updated to 0.2.0-dev.42" });
  await expect(updated.getByTestId("update-game-loading")).toBeVisible();
  // the game was started from Steam before the status came: its event carries the status
  await game(page, "externalStart", "client-steam");
  const button = updated.getByRole("button", { name: "Update game" });
  await expect(button).toBeDisabled();
  await expect(button).toHaveAttribute("title", "Close the game first");
  await expect(updated.getByTestId("update-game-waiting")).toHaveText(`The game is running: Kiln ${BUNDLE} goes into the game when it closes.`);
  await game(page, "openGate", "status");
  await page.waitForTimeout(1000);
  expect(await game(page, "installs")).toEqual([]);
  // Later closes the notice; the install still follows the game's close
  await updated.getByRole("button", { name: "Later" }).click();
  await game(page, "stopGame");
  await expect(toastOk(page)).toBeVisible({ timeout: 15_000 });
  expect(await game(page, "installs")).toEqual([BUNDLE]);
  await expect(page.locator(".k-upd")).toHaveCount(0);
});
