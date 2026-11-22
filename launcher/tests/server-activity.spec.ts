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

test("Start shows its progress at once and the result when the world runs", async ({ page }) => {
  await open(page, "/servers/lab/overview");
  await mock(page, "setServer", { running: false });
  const head = page.locator(".k-server-head");
  await head.getByRole("button", { name: "Start", exact: true }).click();
  const panel = page.locator(".k-sact");
  await expect(panel).toBeVisible();
  await expect(panel.getByText("Starting Lab server")).toBeVisible();
  // the head says so before the next status read
  await expect(head.getByText("Starting.")).toBeVisible();
  // the start task's own lines, as they come
  await expect(panel.locator(".k-sact-line")).toContainText(/started GE-Proton|Loading world|LoadEcsScene|chunks|HostOnline/);
  await shot(page, "1-starting");
  await expect(panel).toHaveAttribute("data-state", "done", { timeout: 20_000 });
  await expect(panel.getByText("Server running")).toBeVisible();
  await panel.getByRole("button", { name: /Show details/ }).click();
  // the task's five progress lines and its last one, oldest first
  const lines = panel.locator(".k-sact-lines li");
  await expect(lines).toHaveCount(6);
  await expect(lines.first()).toContainText("started GE-Proton10-3 ./enshrouded_server.exe");
  await expect(lines.nth(4)).toContainText("812 of 1490 chunks");
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
  await expect(panel.getByText(/^Updating Lab server to Kiln /)).toBeVisible();
  const steps = panel.locator(".k-sact-step");
  await expect(steps).toHaveCount(3);
  await expect(steps.nth(0)).toHaveClass(/running|done/);
  await shot(page, "4-update-running");
  await expect(panel).toHaveAttribute("data-state", "done", { timeout: 30_000 });
  await expect(steps.nth(0)).toHaveClass(/done/);
  await expect(steps.nth(1)).toHaveClass(/done/);
  await expect(steps.nth(2)).toHaveClass(/done/);
  await expect(panel.getByText(/Kiln .* installed/)).toBeVisible();
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
  await expect(panel.getByText("The server folder is read-only: /home/player/srv/enshrouded/lab/kiln")).toBeVisible();
  await expect(panel.locator(".k-sact-step").nth(1)).toHaveClass(/skipped/);
  await expect(panel.getByRole("button", { name: "Try again" })).toBeVisible();
  await expect(panel.getByRole("button", { name: "Open the console" })).toBeVisible();
  await shot(page, "6-update-failed");
});

test("the server list starts a server in place and shows its step and time", async ({ page }) => {
  await open(page, "/servers");
  await mock(page, "setServer", { running: false });
  const row = page.getByRole("table", { name: "Servers" }).getByRole("row").filter({ hasText: "Lab server" });
  await row.getByRole("button", { name: "Start Lab server" }).click();
  await expect(row.locator(".k-sact-cell")).toContainText("Start the server");
  await shot(page, "7-list-starting");
  await expect(row.getByText("Running", { exact: true })).toBeVisible({ timeout: 20_000 });
});
