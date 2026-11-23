// Smoke (browser mock): the launcher update notice, one line and its buttons per state. Found → the notice opens
// under the header → Update → Updating… → Ready to restart (it restarts by itself after a countdown; Restart now) →
// Updated to … → its Kiln goes into the game by itself (the runtime). Also: Skip, a .deb install (Download), the game
// running (Update game held, Kiln goes in when it closes), a failed download (a short toast), Settings and About.
// The restart by itself, its holds and the card before the game's status are in update-apply.spec.ts.
// Screenshots go to $SHOTS_DIR, default .out/launcher/shots/ (SHOTS=0 skips).
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";
import { join } from "node:path";

const SHOTS = process.env.SHOTS_DIR || join(import.meta.dirname, "..", "..", ".out", "launcher", "shots");

async function open(page: Page, route = "/") {
  // these tests click Restart themselves: the countdown to the restart by itself is long here (update-apply.spec.ts)
  await page.addInitScript(() => { (window as unknown as { __kilnRestartDelayMs: number }).__kilnRestartDelayMs = 120_000; });
  await page.goto(`/#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

async function shot(page: Page, name: string) {
  if (process.env.SHOTS === "0") return;
  mkdirSync(SHOTS, { recursive: true });
  await page.waitForTimeout(400);
  await page.screenshot({ path: join(SHOTS, `mock-${name}.png`) });
}

const mock = (page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(([f, a]) => (window as unknown as { __kilnUpdateMock: Record<string, (...x: unknown[]) => unknown> }).__kilnUpdateMock[f as string](...(a as unknown[])), [fn, args] as const);
const game = (page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(([f, a]) => (window as unknown as { __kilnMock: Record<string, (...x: unknown[]) => unknown> }).__kilnMock[f as string](...(a as unknown[])), [fn, args] as const);
const word = (page: Page, w: string) => page.getByRole("button", { name: `Launcher update: ${w}`, exact: true });

test("an update: notice, Update, Restart, then its Kiln goes into the game by itself", async ({ page }) => {
  await open(page);
  await expect(page.locator(".k-upd")).toHaveCount(0);
  await mock(page, "offer", "0.2.0-dev.43", "Notes that the notice does not show.", "0.2.0-dev+20261106.f3e65ec");
  const offer = page.getByRole("dialog", { name: "Kiln 0.2.0-dev.43" });
  await expect(offer).toBeVisible();
  await expect(offer.getByRole("button")).toHaveText(["", "Skip", "Update"]);
  await expect(offer).not.toContainText("Notes");
  await expect(word(page, "Update")).toBeVisible();
  await shot(page, "update-available");

  await offer.getByRole("button", { name: "Update", exact: true }).click();
  const updating = page.getByRole("dialog", { name: "Updating…" });
  await expect(updating.getByRole("progressbar")).toBeVisible();
  await expect(word(page, "Updating")).toBeVisible();
  await shot(page, "update-downloading");
  const ready = page.getByRole("dialog", { name: "Ready to restart" });
  await expect(ready).toBeVisible({ timeout: 8000 });
  await expect(ready.getByRole("button")).toHaveText(["", "Later", "Restart"]);
  await expect(ready.getByTestId("update-restart-in")).toHaveText(/^Restarting by itself in \d+ s$/);
  await expect(word(page, "Restart")).toBeVisible();
  await shot(page, "update-ready");

  await game(page, "setUpgradeAvailable", "0.2.0-dev+20261104.0f1ffa6"); // a bundle the mock has
  // the update writes the active profile again for the new runtime: what that kept is shown (shell/notices.ts)
  await page.evaluate(() => (window as unknown as { __kilnMock: { planActivation: (r: unknown) => void } }).__kilnMock.planActivation({
    retained: [{ path: "mods/XHL-Vein-Mining", reason: "files Kiln placed were changed. It is on in the profile; this changed copy stays as it is, and its loader may still use it.", mayRun: true }] }));
  await ready.getByRole("button", { name: "Restart", exact: true }).click();
  // after the restart the runtime it brought goes into the closed game by itself, through the ordinary install
  const toastOk = page.locator(".k-toast", { hasText: "Kiln 0.2.0-dev+20261104.0f1ffa6 installed" });
  await expect(toastOk).toBeVisible({ timeout: 15_000 });
  await expect(toastOk).toContainText("It came with the launcher update to 0.2.0-dev.43.");
  await expect(page.locator(".k-toast", { hasText: "Some mod folders were kept" })).toContainText("this changed copy stays as it is");
  await shot(page, "update-installed-runtime");
  await expect(page.locator(".k-upd")).toHaveCount(0);
  expect(await game(page, "installs")).toEqual(["0.2.0-dev+20261104.0f1ffa6"]);
  expect(await mock(page, "calls")).toContain("update_ack");
});

test("Skip hides the version until a newer one; Settings can undo", async ({ page }) => {
  await open(page);
  await mock(page, "offer", "0.2.0-dev.43");
  await page.getByRole("dialog", { name: "Kiln 0.2.0-dev.43" }).getByRole("button", { name: "Skip" }).click();
  await expect(page.locator(".k-upd")).toHaveCount(0);
  await open(page, "/settings/online");
  const card = page.locator(".k-card", { has: page.getByRole("heading", { name: "Updates" }) });
  await expect(card.getByText("Skipped 0.2.0-dev.43")).toBeVisible();
  await expect(card.getByRole("switch", { name: /Check automatically/ })).toBeChecked();
  await shot(page, "update-settings");
  await card.getByRole("button", { name: "Undo" }).click();
  await expect(card.getByText("Kiln 0.2.0-dev.43 is available")).toBeVisible();
  await mock(page, "offer", "0.2.0-dev.44");
  await expect(page.getByRole("dialog", { name: "Kiln 0.2.0-dev.44" })).toBeVisible();
});

test("a .deb install offers Download; dismiss keeps the header word", async ({ page }) => {
  await open(page);
  await mock(page, "set", { install: { kind: "deb", inPlace: false, reason: "Installed from a .deb package." } });
  await mock(page, "offer", "0.2.0-dev.43");
  const offer = page.getByRole("dialog", { name: "Kiln 0.2.0-dev.43" });
  await expect(offer.getByRole("button")).toHaveText(["", "Skip", "Download"]);
  await shot(page, "update-deb");
  await offer.getByRole("button", { name: "Dismiss" }).click();
  await expect(offer).toHaveCount(0);
  await word(page, "Update").click();
  await expect(offer).toBeVisible();
});

test("a failed download is a short toast; while the game runs Update game waits, and Kiln goes in when it closes", async ({ page }) => {
  await open(page);
  await mock(page, "failNextInstall", [
    { source: "frostdev.io", step: "signature", message: "the signature does not match the file" },
    { source: "GitHub", step: "download", message: "not reachable" },
  ]);
  await mock(page, "offer", "0.2.0-dev.43");
  await page.getByRole("dialog", { name: "Kiln 0.2.0-dev.43" }).getByRole("button", { name: "Update", exact: true }).click();
  const toast = page.locator(".k-toast-err");
  await expect(toast).toHaveText(/Update failed/, { timeout: 8000 });
  await expect(toast).not.toContainText("signature");
  await expect(page.getByRole("dialog", { name: "Kiln 0.2.0-dev.43" }).getByRole("button", { name: "Update", exact: true })).toBeVisible();
  await shot(page, "update-failed");

  await page.locator(".k-door").click();
  await expect(page.locator(".k-head-state")).toHaveText(/^Running/, { timeout: 8000 });
  await game(page, "setUpgradeAvailable", "0.2.0-dev+20261104.0f1ffa6"); // a bundle the mock has
  await mock(page, "justUpdated", "0.2.0-dev.41");
  const updated = page.getByRole("dialog", { name: "Updated to 0.2.0-dev.42" });
  const button = updated.getByRole("button", { name: "Update game" });
  await expect(button).toBeDisabled();
  await expect(button).toHaveAttribute("title", "Close the game first");
  await expect(updated.getByTestId("update-game-waiting")).toHaveText("The game is running: Kiln 0.2.0-dev+20261104.0f1ffa6 goes into the game when it closes.");
  await shot(page, "update-game-running");
  expect(await game(page, "installs")).toEqual([]);
  await game(page, "stopGame");
  await expect(page.locator(".k-toast", { hasText: "Kiln 0.2.0-dev+20261104.0f1ffa6 installed" })).toBeVisible({ timeout: 15_000 });
  await expect(updated).toHaveCount(0);
  expect(await game(page, "installs")).toEqual(["0.2.0-dev+20261104.0f1ffa6"]);
});

// what a check found, on the line itself (a player could not tell whether Check now had checked at all: the line read
// "Up to date" before and after, and only a short toast said more)
const UP_TO_DATE = /^Kiln is up to date \(0\.2\.0-dev\.42\) · checked \d{1,2}:\d{2}/;
const FAILURES = [
  { source: "frostdev.io", step: "manifest", message: "not reachable (timed out)" },
  { source: "GitHub", step: "manifest", message: "not reachable (dns error: no such host)" },
];
const WHY = "frostdev.io: not reachable (timed out); GitHub: not reachable (dns error: no such host)";

test("About: Check now on a current launcher says Kiln is up to date, its version and the time it checked", async ({ page }) => {
  await open(page, "/about");
  const line = page.getByTestId("update-line");
  await expect(line).toHaveText(UP_TO_DATE);
  // the last check was days ago: this one's time replaces it
  await mock(page, "set", { checked: "2026-11-20T12:00:00Z" });
  await expect(line).toContainText("checked Nov 20");
  await mock(page, "checkDelay", 3000);
  await page.getByRole("button", { name: "Check now" }).click();
  await expect(line).toHaveText("Checking for updates…");
  await expect(page.getByRole("button", { name: "Check now" })).toBeDisabled();
  await expect(line).toHaveText(UP_TO_DATE, { timeout: 8000 });
  await expect(line).not.toContainText("Nov 20");
  await expect(page.locator(".k-toast", { hasText: "Kiln is up to date (0.2.0-dev.42)" })).toBeVisible();
  await shot(page, "update-about-checked");
});

test("About: a failed check says why on the line, and in a toast", async ({ page }) => {
  await open(page, "/about");
  await mock(page, "failNextCheck", FAILURES);
  await page.getByRole("button", { name: "Check now" }).click();
  const line = page.getByTestId("update-line");
  await expect(line.locator(".k-error-text")).toHaveText(new RegExp(`^Check failed: ${WHY.replace(/[().]/g, "\\$&")} · checked \\d{1,2}:\\d{2}`));
  const toast = page.locator(".k-toast-err");
  await expect(toast).toContainText("Check failed");
  await expect(toast).toContainText(WHY);
  await shot(page, "update-about-failed");
  // the next check works again: the line says so
  await page.getByRole("button", { name: "Check now" }).click();
  await expect(line).toHaveText(UP_TO_DATE);
  await expect(line.locator(".k-error-text")).toHaveCount(0);
});

test("Settings: Check now says the same on its row", async ({ page }) => {
  await open(page, "/settings/online");
  const card = page.locator(".k-card", { has: page.getByRole("heading", { name: "Updates" }) });
  const line = card.getByTestId("update-line");
  await expect(line).toHaveText(UP_TO_DATE);
  await mock(page, "failNextCheck", FAILURES);
  await card.getByRole("button", { name: "Check now" }).click();
  await expect(line.locator(".k-error-text")).toContainText(`Check failed: ${WHY}`);
  await expect(page.locator(".k-toast-err")).toContainText(WHY);
  await card.getByRole("button", { name: "Check now" }).click();
  await expect(line).toHaveText(UP_TO_DATE);
  await expect(page.locator(".k-toast", { hasText: "Kiln is up to date (0.2.0-dev.42)" })).toBeVisible();
});

test("Deck: Check says when it checked, why a check failed, and toasts the answer", async ({ page }) => {
  await page.goto("/?deck=steamdeck#/deck/settings");
  const row = page.locator('[data-deck-id="set-update"]');
  await expect(row).toContainText("Up to date");
  await expect(row.locator(".deck-row-sub")).toHaveText(/^Kiln Launcher 0\.2\.0-dev\.42 · checked \d{1,2}:\d{2}/);
  await mock(page, "failNextCheck", FAILURES);
  await row.click();
  await expect(row).toContainText("Check failed");
  await expect(row.locator(".deck-row-sub")).toContainText(`Check failed: ${WHY}`);
  await shot(page, "update-deck-failed");
  await expect(page.locator(".deck-toasts, .deck-dialog, [role=dialog]").filter({ hasText: WHY }).first()).toBeVisible();
  await row.click();
  await expect(row.locator(".deck-row-sub")).toHaveText(/^Kiln Launcher 0\.2\.0-dev\.42 · checked \d{1,2}:\d{2}/);
  await expect(page.locator(".deck-toasts", { hasText: "Kiln is up to date (0.2.0-dev.42)" })).toBeVisible();
});
