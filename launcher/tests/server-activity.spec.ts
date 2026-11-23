// Servers revamp, part 1 (2026-11-22): a server action shows what it does while it runs. Start, Stop, Restart, the game
// update and Install/Update Kiln run as flows (src/pages/servers/serverActions.ts) shown in the activity panel under the
// server's head (ServerActivity.tsx): the title, the time taken, the steps, the newest line of the running step's task,
// and at the end the result or the reason with Try again. Updating Kiln on a running server is one action: stop,
// install, start again. Browser mock (src/api/mock.ts: server_start/stop/restart/update/deploy report the real
// tasks' lines).   bun x playwright test tests/server-activity.spec.ts   (SHOTS_DIR=... keeps screenshots)
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";

const SHOTS = process.env.SHOTS_DIR;
const shot = async (page: Page, name: string) => {
  if (!SHOTS) return;
  mkdirSync(SHOTS, { recursive: true });
  await page.screenshot({ path: `${SHOTS}/${name}.png` });
};

async function open(page: Page, route = "/") {
  await page.goto(`/#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
  if (await page.evaluate((r) => location.hash !== `#${r}`, route)) await page.evaluate((r) => { location.hash = `#${r}`; }, route);
}
const mock = (page: Page, f: string, ...args: unknown[]) =>
  page.evaluate(([f, args]) => (window as unknown as { __kilnMock: Record<string, (...a: unknown[]) => unknown> }).__kilnMock[f as string](...(args as unknown[])), [f, args] as const);

/** Records what Overview's start view says while it shows, each time it changes: the current stage's name and the
 *  screen readers' status line (a stage can be current for well under a second in the mock). */
const recordStart = (page: Page) => page.evaluate(() => {
  const w = window as unknown as { __seen: { stages: string[]; status: string[] } };
  w.__seen = { stages: [], status: [] };
  const push = (list: string[], v: string | null | undefined) => { if (v && list[list.length - 1] !== v) list.push(v); };
  new MutationObserver(() => {
    push(w.__seen.stages, document.querySelector(".k-strack li.now b")?.textContent);
    push(w.__seen.status, document.querySelector(".k-sov-start > [role=status]")?.textContent);
  }).observe(document.body, { subtree: true, childList: true, attributes: true, characterData: true });
});
const seen = (page: Page) => page.evaluate(() => (window as unknown as { __seen: { stages: string[]; status: string[] } }).__seen);

test("Overview shows the start's five stages, the clock and the newest line while it starts", async ({ page }) => {
  await open(page, "/servers/lab/overview");
  await mock(page, "setServer", { running: false });
  const head = page.locator(".k-server-head");
  await expect(head.locator(".k-server-word")).toHaveText("Stopped");
  await recordStart(page);
  await head.getByRole("button", { name: "Start", exact: true }).click();
  const track = page.getByRole("list", { name: "Start stages" });
  await expect(track.getByRole("listitem")).toHaveText([/Preparing/, /Wine/, /Server process/, /Loading the world/, /Running/]);
  await expect(head.locator(".k-server-word")).toHaveText("Starting");
  await expect(page.locator(".k-sclock")).toBeVisible();
  // the mock's start names its stages as the backend does: the stage moves to loading the world
  await expect(track.locator("li.now")).toContainText("Loading the world", { timeout: 10_000 });
  await expect(page.locator(".k-slastline")).not.toBeEmpty();
  // screen readers hear which stages are done, which one runs, which wait
  await expect(track.getByRole("listitem").first()).toContainText("(done)");
  await expect(track.locator("li.now")).toContainText("(running)");
  await expect(track.getByRole("listitem").last()).toContainText(/\((waiting|running)\)/);
  await shot(page, "8-overview-starting");
  // the activity panel is not shown twice on Overview
  await expect(page.locator(".k-sact")).toHaveCount(0);
  await expect(head.locator(".k-server-word")).toHaveText("Running", { timeout: 20_000 });
  // every stage was current in turn ("Server process" too: spawned, no log yet), and the status line said each
  const s = await seen(page);
  expect(s.stages).toEqual(["Preparing", "Wine", "Server process", "Loading the world", "Running"]);
  expect(s.status[0]).toBe("Lab server is starting: Preparing.");
  expect(s.status).toContain("Lab server is starting: Loading the world.");
  // the start that ended shows in the panel now: its result, for a while
  await expect(page.locator(".k-sact")).toHaveAttribute("data-state", "done");
  await expect(page.locator(".k-sstats")).toBeVisible();
  await expect(page.getByRole("navigation", { name: "Sections" }).getByRole("link")).toHaveCount(5);
  await shot(page, "9-overview-running");
});

test("Overview reads the stages from the start's lines when the backend names none", async ({ page }) => {
  await open(page, "/servers/lab/overview");
  await mock(page, "setServer", { running: false });
  await mock(page, "namedStages", false);
  await recordStart(page);
  const head = page.locator(".k-server-head");
  await head.getByRole("button", { name: "Start", exact: true }).click();
  await expect(page.getByRole("list", { name: "Start stages" }).locator("li.now")).toContainText("Loading the world", { timeout: 10_000 });
  await expect(head.locator(".k-server-word")).toHaveText("Running", { timeout: 20_000 });
  // "started ... (wine pid N)" as the newest line: the process; the game's lines after it: the world
  expect((await seen(page)).stages).toEqual(["Preparing", "Server process", "Loading the world"]);
});

test("a restart on Overview stops first (no start stage is current then), and the start's clock is the start's own", async ({ page }) => {
  await open(page, "/servers/lab/overview");
  const head = page.locator(".k-server-head");
  await expect(head.locator(".k-server-word")).toHaveText("Running");
  await recordStart(page);
  await head.getByRole("button", { name: "Restart", exact: true }).click();
  const track = page.getByRole("list", { name: "Start stages" });
  await expect(track.getByRole("listitem")).toHaveText([/Stopping/, /Preparing/, /Wine/, /Server process/, /Loading the world/, /Running/]);
  await expect(head.locator(".k-server-word")).toHaveText("Running", { timeout: 20_000 });
  const s = await seen(page);
  expect(s.stages).toEqual(["Stopping", "Preparing", "Wine", "Server process", "Loading the world", "Running"]);
  expect(s.status[0]).toBe("Lab server is restarting: it stops first.");
  await expect(page.locator(".k-sact")).toHaveAttribute("data-flow", "restart");
});

test("a start that fails on Overview says why, with Try again, in the panel (no stage view left, no silence)", async ({ page }) => {
  await open(page, "/servers/lab/overview");
  await mock(page, "setServer", { running: false });
  const why = "lab exited during the start: exit code 3; see the Console";
  await mock(page, "holdTasks", "server_start", { gate: "start-fail", fail: why });
  await mock(page, "openGate", "start-fail");
  const head = page.locator(".k-server-head");
  await head.getByRole("button", { name: "Start", exact: true }).click();
  await expect(page.getByRole("list", { name: "Start stages" })).toBeVisible();
  // the running start is the stage view's; the panel waits
  await expect(page.locator(".k-sact")).toHaveCount(0);
  const panel = page.locator(".k-sact");
  await expect(panel).toHaveAttribute("data-state", "failed", { timeout: 20_000 });
  await expect(panel.locator(".k-sact-error")).toContainText(why);
  await expect(panel.getByRole("status")).toHaveText(`Starting Lab server failed: ${why}`);
  await expect(page.getByRole("list", { name: "Start stages" })).toHaveCount(0);
  await expect(head.locator(".k-server-word")).toHaveText("Stopped");
  await shot(page, "11-overview-start-failed");
  await mock(page, "holdTasks", "server_start", null);
  await panel.getByRole("button", { name: "Try again" }).click();
  await expect(page.getByRole("list", { name: "Start stages" })).toBeVisible();
  await expect(head.locator(".k-server-word")).toHaveText("Running", { timeout: 20_000 });
  await expect(panel).toHaveAttribute("data-state", "done");
});

test.describe("at 1024x640", () => {
  test.use({ viewport: { width: 1024, height: 640 } });
  test("the start view stacks: the stages, the clock and the console keep their height above the aside", async ({ page }) => {
    await open(page, "/servers/lab/overview");
    await mock(page, "setServer", { running: false });
    await mock(page, "holdTasks", "server_start", { gate: "small-start" });
    await page.locator(".k-server-head").getByRole("button", { name: "Start", exact: true }).click();
    await expect(page.getByRole("list", { name: "Start stages" })).toBeVisible();
    await expect(page.locator(".k-sov-live .k-console-log")).toBeVisible();
    const live = (await page.locator(".k-sov-live").boundingBox())!;
    const aside = (await page.locator(".k-sov-aside").boundingBox())!;
    const log = (await page.locator(".k-sov-live .k-console-log").boundingBox())!;
    expect(live.height).toBeGreaterThan(300);
    expect(aside.y).toBeGreaterThanOrEqual(live.y + live.height - 1);
    expect(log.height).toBeGreaterThanOrEqual(199);
    await shot(page, "12-overview-starting-1024x640");
    await mock(page, "openGate", "small-start");
  });
});

test("Start shows its progress at once and the result when the world runs", async ({ page }) => {
  // the activity panel shows on every section but Overview (Overview shows the start's stages)
  await open(page, "/servers/lab/backups");
  await mock(page, "setServer", { running: false });
  const head = page.locator(".k-server-head");
  await head.getByRole("button", { name: "Start", exact: true }).click();
  const panel = page.locator(".k-sact");
  await expect(panel).toBeVisible();
  await expect(panel.locator(".k-sact-title")).toHaveText("Starting Lab server");
  // the head says so at once, from the start's step: the mock's status says starting only 1.5 s later, as the real one
  // does once the process is there
  await expect(head.getByText("Starting.")).toBeVisible({ timeout: 1000 });
  // the start task's own lines, as they come
  await expect(panel.locator(".k-sact-line")).toContainText(/started GE-Proton|Loading world|LoadEcsScene|chunks|HostOnline/);
  await shot(page, "1-starting");
  // details opened while it starts stay open when the server goes live (the page's frame remounts then)
  await panel.getByRole("button", { name: /Show details/ }).click();
  await expect(panel.locator(".k-sact-lines")).toBeVisible();
  await expect(panel).toHaveAttribute("data-state", "done", { timeout: 20_000 });
  await expect(panel.locator(".k-sact-result")).toHaveText("Server running");
  await expect(panel.locator(".k-sact-lines")).toBeVisible();
  // the task's lines, oldest first: its named stages' lines (preparing ... running) and its last one
  const lines = panel.locator(".k-sact-lines li");
  await expect(lines).toHaveCount(8);
  await expect(lines.first()).toContainText("checking the server folder");
  await expect(panel.locator(".k-sact-lines")).toContainText("started GE-Proton10-3 ./enshrouded_server.exe");
  await expect(panel.locator(".k-sact-lines")).toContainText("812 of 1490 chunks");
  await shot(page, "2-started");
});

test("Update Kiln on a running server is one action: stop, install, start again, each step shown", async ({ page }) => {
  await open(page, "/servers/lab/overview");
  await page.getByRole("button", { name: "More server actions" }).click();
  await page.getByRole("menuitem", { name: "Update Kiln…" }).click();
  const dlg = page.getByRole("dialog", { name: "Update Kiln" });
  await expect(dlg.getByText(/The server is running: it is stopped/)).toBeVisible();
  const plan = dlg.getByRole("list", { name: "What happens" });
  await expect(plan.getByRole("listitem")).toHaveText(["Stop the server", /^Install Kiln /, "Start it again"]);
  await shot(page, "3-update-dialog");
  await dlg.getByRole("button", { name: "Update", exact: true }).click();
  const panel = page.locator(".k-sact");
  await expect(panel.locator(".k-sact-title")).toHaveText(/^Updating Lab server to Kiln /);
  const steps = panel.locator(".k-sact-step");
  await expect(steps).toHaveCount(3);
  await expect(steps.nth(0)).toHaveClass(/running|done/);
  await shot(page, "4-update-running");
  await expect(panel).toHaveAttribute("data-state", "done", { timeout: 30_000 });
  await expect(steps.nth(0)).toHaveClass(/done/);
  await expect(steps.nth(1)).toHaveClass(/done/);
  await expect(steps.nth(2)).toHaveClass(/done/);
  await expect(panel.locator(".k-sact-result")).toContainText(/Kiln .* installed/);
  await shot(page, "5-update-done");
});

test("a failed install says why, skips the start, and offers Try again and the console", async ({ page }) => {
  await open(page, "/servers/lab/overview");
  await mock(page, "setServer", { running: false });
  await mock(page, "holdTasks", "server_deploy", { gate: "deploy", fail: "The server folder is read-only: /home/player/srv/enshrouded/lab/kiln" });
  await mock(page, "openGate", "deploy");
  await page.getByRole("button", { name: "More server actions" }).click();
  await page.getByRole("menuitem", { name: "Update Kiln…" }).click();
  const dlg = page.getByRole("dialog", { name: "Update Kiln" });
  await dlg.getByText("Start the server afterwards").click();
  await expect(dlg.getByRole("list", { name: "What happens" }).getByRole("listitem")).toHaveText([/^Install Kiln /, "Start the server"]);
  await dlg.getByRole("button", { name: "Update", exact: true }).click();
  const panel = page.locator(".k-sact");
  await expect(panel).toHaveAttribute("data-state", "failed", { timeout: 20_000 });
  await expect(panel.locator(".k-sact-error")).toContainText("The server folder is read-only: /home/player/srv/enshrouded/lab/kiln");
  // screen readers hear the failure once, in the summary
  await expect(panel.getByRole("status")).toHaveText(/failed: The server folder is read-only/);
  await expect(panel.locator(".k-sact-step").nth(1)).toHaveClass(/skipped/);
  await expect(panel.getByRole("button", { name: "Try again" })).toBeVisible();
  await expect(panel.getByRole("button", { name: "Open the console" })).toBeVisible();
  await shot(page, "6-update-failed");
  // the console page, not Overview
  await panel.getByRole("button", { name: "Open the console" }).click();
  await expect(page).toHaveURL(/#\/servers\/lab\/console$/);
});

test("Stop during a start takes over: the start stops waiting and the server stops", async ({ page }) => {
  // the activity panel (with Stop waiting) shows on every section but Overview
  await open(page, "/servers/lab/backups");
  await mock(page, "setServer", { running: false });
  const head = page.locator(".k-server-head");
  await head.getByRole("button", { name: "Start", exact: true }).click();
  const panel = page.locator(".k-sact");
  await expect(panel.getByRole("button", { name: "Stop waiting" })).toBeVisible();
  await head.getByRole("button", { name: "Stop", exact: true }).click();
  await expect(panel.locator(".k-sact-title")).toHaveText("Stopping Lab server");
  await expect(panel).toHaveAttribute("data-state", "done", { timeout: 20_000 });
  await expect(panel.locator(".k-sact-result")).toHaveText("Server stopped");
  await expect(head.getByRole("button", { name: "Start", exact: true })).toBeEnabled();
  // the interrupted start's late failure changes nothing
  await page.waitForTimeout(4000);
  await expect(panel).toHaveAttribute("data-flow", "stop");
  await expect(head.getByText(/^Stopped\./)).toBeVisible();
});

test("the server list starts a server in place and shows its step and time", async ({ page }) => {
  await open(page, "/servers");
  await mock(page, "setServer", { running: false });
  const row = page.getByRole("table", { name: "Servers" }).getByRole("row").filter({ hasText: "Lab server" });
  // the keyboard presses the row's button; it does not open the row
  await row.getByRole("button", { name: "Start Lab server" }).focus();
  await page.keyboard.press("Enter");
  await expect(page).toHaveURL(/#\/servers$/);
  await expect(row.locator(".k-sact-cell")).toContainText("Start the server");
  await shot(page, "7-list-starting");
  await expect(row.getByText("Running", { exact: true })).toBeVisible({ timeout: 20_000 });
});
