// Servers revamp, the list (2026-11-22, mockup A "Hearth dashboard" with mockup B's live row): the Servers page says
// what runs and what needs a Kiln update in one sentence, offers the update in a banner that runs the Update Kiln flow
// (stop, install, start again when it ran), shows each server's running flow live in its row (state word, cones, the
// running step and its time), has quick actions and a ··· menu per row, a "Recent" list from the launcher-side
// activity log, and the new-server wizard (where and what, install, Install Kiln, Start). Browser mock with
// ?servers=3 (src/api/mock.ts: Ember Isles runs with 3 players, Hollow Lab is stopped with Kiln 0.2.0-dev.30.2,
// Frostwood runs on vps-01 over SSH; the runtime list has 0.2.0-dev.30.4).
//   KILN_DEV_PORT=1521 bun x playwright test tests/servers-list.spec.ts   (SHOTS_DIR=... keeps 1440x900 and 1280x800 screenshots)
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";

const SHOTS = process.env.SHOTS_DIR;
const shot = async (page: Page, name: string) => {
  if (!SHOTS) return;
  mkdirSync(SHOTS, { recursive: true });
  const v = page.viewportSize();
  await page.screenshot({ path: `${SHOTS}/${name}-${v?.width}x${v?.height}.png` });
};

/** What "Recent" holds before the page loads (the mockup's story), newest first. */
const STORY = (now: number) => [
  { at: now - 9 * 60_000, serverId: "hollow-lab", name: "Hollow Lab", kind: "stop", state: "done", ms: 900, steps: ["stop"] },
  { at: now - 41 * 60_000, serverId: "ember-isles", name: "Ember Isles", kind: "backup", state: "done", ms: 2_100, steps: ["backup"] },
  { at: now - 134 * 60_000, serverId: "ember-isles", name: "Ember Isles", kind: "start", state: "done", ms: 41_000, steps: ["start"], kiln: "0.2.0-dev.30.4" },
  { at: now - 135 * 60_000, serverId: "ember-isles", name: "Ember Isles", kind: "deploy", state: "done", ms: 64_000, steps: ["stop", "deploy", "start"], kiln: "0.2.0-dev.30.4", from: "0.2.0-dev.30.2" },
];

async function open(page: Page, opts: { story?: boolean; route?: string } = {}) {
  if (opts.story) await page.addInitScript((entries) => localStorage.setItem("kiln.servers.activity", JSON.stringify(entries)), STORY(Date.now()));
  await page.goto(`/?servers=3#${opts.route ?? "/servers"}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.getByRole("table", { name: "Servers" })).toBeVisible();
}
const mock = (page: Page, f: string, ...args: unknown[]) =>
  page.evaluate(([f, args]) => (window as unknown as { __kilnMock: Record<string, (...a: unknown[]) => unknown> }).__kilnMock[f as string](...(args as unknown[])), [f, args] as const);
const row = (page: Page, name: string) => page.getByRole("table", { name: "Servers" }).getByRole("row").filter({ hasText: name });
const say = (page: Page) => page.locator(".k-page-head .k-page-sub");
const recent = (page: Page) => page.getByRole("list", { name: "Recent actions" }).getByRole("listitem");

test.use({ viewport: { width: 1440, height: 900 } });

test("the list says what runs and what needs an update, and a start shows live in its row", async ({ page }) => {
  await open(page, { story: true });
  await expect(say(page)).toHaveText("3 servers. Ember Isles is running with 3 players; Hollow Lab has a Kiln update.");
  const ember = row(page, "Ember Isles"), hollow = row(page, "Hollow Lab"), frost = row(page, "Frostwood");
  await expect(ember).toContainText("Running");
  await expect(ember).toContainText("2 h 14 min");
  await expect(ember).toContainText("3 / 16");
  await expect(ember).toContainText(/59\.\d\/s/);
  await expect(ember).toContainText("0.2.0-dev.30.4");
  await expect(hollow).toContainText("Stopped");
  // the log's last stop says since when
  await expect(hollow).toContainText(/since \d\d:\d\d/);
  await expect(hollow).toContainText("0.2.0-dev.30.2");
  await expect(hollow.getByText("Update", { exact: true })).toBeVisible();
  await expect(frost).toContainText("vps-01 over SSH");
  await expect(frost).toContainText("0 / 8");
  // remote: no health, no Kiln update
  await expect(frost.getByRole("button", { name: /Update Kiln/ })).toHaveCount(0);
  await expect(recent(page)).toHaveCount(4);
  await expect(recent(page).first()).toContainText("Hollow LabStopped by you. Saved in 0.9 s.");
  await expect(recent(page).nth(2)).toContainText("Started in 41 s, Kiln 0.2.0-dev.30.4.");
  await shot(page, "1-list");

  await hollow.getByRole("button", { name: "Start Hollow Lab" }).click();
  const live = hollow.locator(".k-srow-live");
  await expect(live).toBeVisible({ timeout: 1000 });
  await expect(live).toHaveAttribute("data-flow", "start");
  await expect(live).toContainText("Starting");
  await expect(live).toContainText("Start the server");
  await expect(live.locator(".k-srow-time")).toHaveText(/^\d+ s$/);
  await expect(live.locator(".k-cones-s .k-cone")).toHaveCount(3);
  await expect(say(page)).toContainText("Hollow Lab is starting");
  await expect(page).toHaveURL(/#\/servers$/);
  await shot(page, "2-list-starting");
  await expect(hollow.getByText("Running", { exact: true })).toBeVisible({ timeout: 20_000 });
  await expect(recent(page).first()).toContainText(/Hollow LabStarted in [\d.]+ s, Kiln 0\.2\.0-dev\.30\.2\./);
  await expect(hollow).not.toContainText("since");
});

test("the update banner updates Hollow Lab: stop, install, start again, each step live in its row", async ({ page }) => {
  await open(page);
  const hollow = row(page, "Hollow Lab");
  const banner = page.locator(".k-banner");
  await expect(banner).toContainText("Kiln 0.2.0-dev.30.4 is out. Hollow Lab runs 0.2.0-dev.30.2; it is stopped, so only Kiln changes.");
  // running, the update stops it and starts it again
  await hollow.getByRole("button", { name: "Start Hollow Lab" }).click();
  await expect(hollow.getByText("Running", { exact: true })).toBeVisible({ timeout: 20_000 });
  await expect(banner).toContainText("it is running, so it stops, updates and starts again.");
  await shot(page, "3-update-banner");
  for (const [kind, gate] of [["server_stop", "u-stop"], ["server_deploy", "u-deploy"], ["server_start", "u-start"]]) await mock(page, "holdTasks", kind, { gate });
  await banner.getByRole("button", { name: "Update Hollow Lab" }).click();
  const live = hollow.locator(".k-srow-live");
  await expect(live).toHaveAttribute("data-flow", "deploy");
  await expect(live).toContainText("Stopping");
  await expect(live).toContainText("1 of 3: Stop the server");
  await expect(banner).toContainText("Updating Hollow Lab, step 1 of 3: Stop the server.");
  await expect(live.locator(".k-cones-s .k-cone")).toHaveCount(3);
  await mock(page, "openGate", "u-stop");
  await expect(live).toContainText("2 of 3: Install Kiln 0.2.0-dev.30.4");
  await expect(live).toContainText("Updating");
  await expect(live.locator(".k-cone.done")).toHaveCount(1);
  await expect(banner).toContainText("step 2 of 3: Install Kiln 0.2.0-dev.30.4.");
  await shot(page, "4-update-running");
  await mock(page, "openGate", "u-deploy");
  await expect(live).toContainText("3 of 3: Start it again");
  await expect(live).toContainText("Starting");
  await mock(page, "openGate", "u-start");
  await expect(hollow.getByText("Running", { exact: true })).toBeVisible({ timeout: 20_000 });
  await expect(banner).toHaveCount(0);
  await expect(hollow).toContainText("0.2.0-dev.30.4");
  await expect(hollow.getByText("Update", { exact: true })).toHaveCount(0);
  await expect(say(page)).not.toContainText("Kiln update");
  await expect(recent(page).first()).toContainText(/Kiln updated from 0\.2\.0-dev\.30\.2 to 0\.2\.0-dev\.30\.4 \(stop, install, start: [\d.]+ s\)\./);
  await shot(page, "5-update-done");
});

test("a failed update says why in the banner and tries again from the failed step", async ({ page }) => {
  await open(page);
  await mock(page, "holdTasks", "server_deploy", { gate: "f-deploy", fail: "The downloaded bundle failed its check (sha256 mismatch)" });
  await mock(page, "openGate", "f-deploy");
  const banner = page.locator(".k-banner");
  await banner.getByRole("button", { name: "Update Hollow Lab" }).click();
  await expect(banner).toContainText("The Kiln update of Hollow Lab failed: The downloaded bundle failed its check (sha256 mismatch)", { timeout: 20_000 });
  await expect(row(page, "Hollow Lab").getByLabel(/The last action failed/)).toBeVisible();
  await expect(recent(page).first()).toContainText("Kiln install failed after");
  await mock(page, "holdTasks", "server_deploy", null);
  await banner.getByRole("button", { name: "Try again" }).click();
  await expect(banner).toHaveCount(0, { timeout: 20_000 });
  await expect(row(page, "Hollow Lab")).toContainText("0.2.0-dev.30.4");
});

test("the ··· menu: Restart, Open the console, Back up now, Update Kiln, Remove from the launcher", async ({ page }) => {
  await open(page);
  await row(page, "Ember Isles").getByRole("button", { name: "More actions for Ember Isles" }).click();
  const items = page.getByRole("menu").getByRole("menuitem");
  await expect(items).toHaveText(["Restart", "Open the console", "Back up now", "Update Kiln", "Remove from the launcher"]);
  await expect(page.getByRole("menu").getByRole("separator")).toHaveCount(2);
  // Ember Isles has the newest Kiln and runs: no update, no remove
  await expect(items.nth(3)).toBeDisabled();
  await expect(items.nth(4)).toBeDisabled();
  await page.getByRole("menu").evaluate((el) => Promise.all(el.getAnimations().map((a) => a.finished)));
  await shot(page, "6-menu");
  await mock(page, "holdTasks", "server_backup_create", { gate: "bk" });
  await items.filter({ hasText: "Back up now" }).click();
  const live = row(page, "Ember Isles").locator(".k-srow-live");
  await expect(live).toContainText("Backing up");
  await expect(live).toContainText("Back up the save");
  await mock(page, "openGate", "bk");
  await expect(recent(page).first()).toContainText(/Ember IslesBacked up in \d/, { timeout: 10_000 });
  await expect(page).toHaveURL(/#\/servers$/);

  // Hollow Lab: the update with its version, and Remove (typed)
  await row(page, "Hollow Lab").getByRole("button", { name: "More actions for Hollow Lab" }).click();
  await expect(items.nth(3)).toHaveText("Update Kiln0.2.0-dev.30.4");
  await expect(items.nth(3)).toBeEnabled();
  await items.filter({ hasText: "Remove from the launcher" }).click();
  const dlg = page.getByRole("dialog", { name: "Remove this server?" });
  await dlg.getByRole("textbox").fill("hollow-lab");
  await dlg.getByRole("button", { name: "Remove" }).click();
  await expect(row(page, "Hollow Lab")).toHaveCount(0);
  await expect(say(page)).toHaveText("2 servers. Ember Isles is running with 3 players.");
  await expect(recent(page).first()).toContainText("Hollow LabRemoved from the launcher. Its files stay on disk.");

  // a right click on a row opens its menu; a remote server has no Update Kiln
  await row(page, "Frostwood").click({ button: "right" });
  await expect(items).toHaveText(["Restart", "Open the console", "Back up now", "Remove from the launcher"]);
  await items.filter({ hasText: "Open the console" }).click();
  await expect(page).toHaveURL(/#\/servers\/frostwood\/overview$/);
});

test("keyboard: Enter opens a row; its buttons and its menu keep their own keys", async ({ page }) => {
  await open(page);
  const more = page.getByRole("button", { name: "More actions for Ember Isles" });
  await more.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByRole("menuitem", { name: "Restart" })).toBeFocused();
  await page.keyboard.press("Escape");
  await expect(page.getByRole("menu")).toHaveCount(0);
  await expect(page).toHaveURL(/#\/servers$/);
  await page.getByRole("button", { name: "Start Hollow Lab" }).focus();
  await page.keyboard.press("Space");
  await expect(row(page, "Hollow Lab").locator(".k-srow-live")).toBeVisible();
  await expect(page).toHaveURL(/#\/servers$/);
  // Tab reaches the row itself; Enter there opens the server
  await row(page, "Frostwood").focus();
  await page.keyboard.press("Enter");
  await expect(page).toHaveURL(/#\/servers\/frostwood\/overview$/);
});

test("the wizard: where and what, install, Install Kiln, Start, then the new server's row and Recent", async ({ page }) => {
  await open(page);
  await page.getByRole("button", { name: "New server" }).click();
  const dlg = page.getByRole("dialog", { name: "New server" });
  const install = dlg.getByRole("button", { name: "Install", exact: true });
  await expect(dlg.getByRole("list", { name: "Steps" }).getByRole("listitem")).toHaveText(["1 Where and what", "2 Install", "3 Install Kiln", "4 Start (not chosen)"]);
  await expect(install).toBeDisabled();
  // the id follows the name until it is typed; every check of the old dialog stays
  await dlg.getByLabel("Name", { exact: true }).fill("Ember Isles 2");
  const id = dlg.getByLabel("Id", { exact: true });
  await expect(id).toHaveValue("ember-isles-2");
  await id.fill("Ember Isles");
  await expect(dlg.getByText("Not a valid id")).toBeVisible();
  await id.fill("ember-isles");
  await expect(dlg.getByText("Ember Isles has this id already.")).toBeVisible();
  // emptied, it follows the name again
  await id.fill("");
  await dlg.getByLabel("Name", { exact: true }).fill("Ember Isles Two");
  await expect(id).toHaveValue("ember-isles-two");
  await dlg.getByLabel("Name", { exact: true }).fill("Ember Isles 2");
  await expect(id).toHaveValue("ember-isles-2");
  const dir = dlg.getByLabel("Folder", { exact: true });
  await dir.fill("srv/ember-isles-2");
  await dir.press("Enter");
  await expect(dlg.getByText(/An absolute folder/)).toBeVisible();
  await expect(install).toBeDisabled();
  await dir.fill("/home/player/srv/enshrouded/ember-isles-2");
  await dir.press("Enter");
  // the first port no server here uses
  const port = dlg.getByLabel("UDP port");
  await expect(port).toHaveValue("15641");
  await expect(dlg.getByText("Free", { exact: true })).toBeVisible();
  await port.fill("80");
  await port.press("Enter");
  await expect(dlg.getByText("A port from 1024 to 65535.")).toBeVisible();
  await expect(install).toBeDisabled();
  await port.fill("15637");
  await port.press("Enter");
  await expect(dlg.getByText("Ember Isles uses this port too: the two cannot run at the same time.")).toBeVisible();
  await port.fill("15641");
  await port.press("Enter");
  await dlg.getByLabel("Copy game files from").selectOption("ember-isles");
  await expect(dlg.getByText("Skip SteamCMD")).toBeVisible();
  await expect(dlg.getByRole("switch", { name: /Install Kiln 0\.2\.0-dev\.30\.4/ })).toBeChecked();
  await dlg.getByText("Start the server when it is ready").click();
  await expect(dlg.getByRole("list", { name: "What happens" }).getByRole("listitem"))
    .toHaveText(["Copy the game files and check them (SteamCMD)", "Install Kiln 0.2.0-dev.30.4", "Start the server"]);
  await expect(install).toBeEnabled();
  await shot(page, "7-wizard-where");

  await mock(page, "holdTasks", "server_create", { gate: "create" });
  await install.click();
  const run = page.getByRole("dialog", { name: "Installing Ember Isles 2" });
  await expect(run).toBeVisible();
  await expect(run.getByRole("list", { name: "Steps" }).getByRole("listitem").nth(1)).toHaveAttribute("aria-current", "step");
  const steps = run.locator(".k-wiz-step");
  await expect(steps).toHaveCount(3);
  await expect(steps.nth(0)).toHaveClass(/running/);
  await expect(steps.nth(0)).toContainText("Copy the game files and check them (SteamCMD)");
  await expect(steps.nth(0)).toContainText("copying the game files from /home/player/srv/enshrouded/ember-isles (reflink)");
  await expect(steps.nth(0)).toContainText("creating the Wine prefix (wineboot)", { timeout: 15_000 });
  await expect(run.locator(".k-wiz-clock-n")).toHaveText(/^\d+ s$/);
  // the new server is in the list already, its row live
  await expect(row(page, "Ember Isles 2").locator(".k-srow-live")).toContainText("Installing");
  await shot(page, "8-wizard-install");
  await mock(page, "openGate", "create");
  await expect(steps.nth(1)).toHaveClass(/running|done/, { timeout: 15_000 });
  await expect(run.getByRole("list", { name: "Steps" }).getByRole("listitem").nth(2)).toHaveClass(/on|done/);
  await expect(run.locator(".k-wiz-progress")).toHaveAttribute("data-state", "done", { timeout: 30_000 });
  await expect(steps).toHaveClass([/done/, /done/, /done/]);
  await expect(run.getByText("The server is ready and running.")).toBeVisible();
  await shot(page, "9-wizard-done");
  await run.getByRole("button", { name: "Close" }).click();
  await expect(row(page, "Ember Isles 2").getByText("Running", { exact: true })).toBeVisible();
  await expect(row(page, "Ember Isles 2")).toContainText("0.2.0-dev.30.4");
  await expect(recent(page).first()).toContainText(/Ember Isles 2Installed with Kiln 0\.2\.0-dev\.30\.4 and started in [\d.]+ s\./);
  // a new wizard starts afresh
  await page.getByRole("button", { name: "New server" }).click();
  await expect(page.getByRole("dialog", { name: "New server" }).getByLabel("Name", { exact: true })).toHaveValue("");
});

test("a failed install says why, skips the rest, and Back keeps what was typed", async ({ page }) => {
  await open(page);
  const why = "steamcmd: Disk write failure: not enough space in /home/player/srv (needs 3.1 GB, 0.4 GB free)";
  await mock(page, "holdTasks", "server_create", { gate: "create-fail", fail: why });
  await mock(page, "openGate", "create-fail");
  await page.getByRole("button", { name: "New server" }).click();
  const dlg = page.getByRole("dialog", { name: "New server" });
  await dlg.getByLabel("Name", { exact: true }).fill("Frost Lab");
  const dir = dlg.getByLabel("Folder", { exact: true });
  await dir.fill("/home/player/srv/enshrouded/frost-lab");
  await dir.press("Enter");
  await dlg.getByRole("button", { name: "Install", exact: true }).click();
  const run = page.getByRole("dialog", { name: "Installing Frost Lab" });
  const progress = run.locator(".k-wiz-progress");
  await expect(progress).toHaveAttribute("data-state", "failed", { timeout: 20_000 });
  await expect(run.locator(".k-wiz-error")).toContainText("It stopped at: Install the server files (SteamCMD).");
  await expect(run.locator(".k-wiz-error")).toContainText(why);
  await expect(run.getByRole("status")).toHaveText(/failed: steamcmd: Disk write failure/);
  await expect(run.locator(".k-wiz-step").nth(0)).toHaveClass(/failed/);
  await expect(run.locator(".k-wiz-step").nth(1)).toHaveClass(/skipped/);
  await expect(run.getByRole("list", { name: "Steps" }).getByRole("listitem").nth(1)).toHaveClass(/failed/);
  // the instance file was written first: the server is listed, half made, and the wizard says so
  await expect(run.locator(".k-wiz-error")).toContainText("The launcher now lists frost-lab, half made.");
  await expect(row(page, "Frost Lab")).toHaveCount(1);
  await shot(page, "10-wizard-failed");
  await run.getByRole("button", { name: "Back" }).click();
  const back = page.getByRole("dialog", { name: "New server" });
  await expect(back.getByLabel("Name", { exact: true })).toHaveValue("Frost Lab");
  await expect(back.getByLabel("Folder", { exact: true })).toHaveValue("/home/player/srv/enshrouded/frost-lab");
  await expect(back.getByText("Frost Lab has this id already.")).toBeVisible();
  await expect(recent(page).first()).toContainText(/Frost LabInstall failed after [\d.]+ s: steamcmd: Disk write failure/);
});

test.describe("at 1280x800 (the Steam Deck and the launcher's default window)", () => {
  test.use({ viewport: { width: 1280, height: 800 } });
  test("the list, a live row and the wizard fit", async ({ page }) => {
    await open(page, { story: true });
    await expect(say(page)).toHaveText("3 servers. Ember Isles is running with 3 players; Hollow Lab has a Kiln update.");
    // every column and the quick actions fit without a horizontal scroll
    const scroll = page.locator(".k-table-scroll");
    expect(await scroll.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
    await expect(row(page, "Frostwood").getByRole("button", { name: "More actions for Frostwood" })).toBeInViewport();
    await shot(page, "1-list");
    await mock(page, "holdTasks", "server_start", { gate: "deck-start" });
    await row(page, "Hollow Lab").getByRole("button", { name: "Start Hollow Lab" }).click();
    await expect(row(page, "Hollow Lab").locator(".k-srow-live")).toContainText("Start the server");
    await shot(page, "2-list-starting");
    await page.getByRole("button", { name: "New server" }).click();
    const dlg = page.getByRole("dialog", { name: "New server" });
    await dlg.getByLabel("Name", { exact: true }).fill("Ember Isles 2");
    await dlg.getByLabel("Folder", { exact: true }).fill("/home/player/srv/enshrouded/ember-isles-2");
    await dlg.getByLabel("Folder", { exact: true }).press("Enter");
    await expect(dlg.getByRole("button", { name: "Install", exact: true })).toBeInViewport();
    await shot(page, "7-wizard-where");
    await mock(page, "holdTasks", "server_create", { gate: "deck-create" });
    await dlg.getByRole("button", { name: "Install", exact: true }).click();
    const run = page.getByRole("dialog", { name: "Installing Ember Isles 2" });
    await expect(run.locator(".k-wiz-step").nth(0)).toContainText("creating the Wine prefix (wineboot)", { timeout: 15_000 });
    await shot(page, "8-wizard-install");
    await mock(page, "openGate", "deck-create");
    await mock(page, "openGate", "deck-start");
  });
});
