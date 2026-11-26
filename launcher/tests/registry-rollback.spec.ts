// The Conflicts view's registry plan card: kiln_manage::registry_plan through the launcher's
// registry_rollback command. The dry run's changes are shown before anything is written; Clear asks first too.
import { expect, test, type Page } from "@playwright/test";
import { registryFixture } from "./fixtures/registryReport";

const A = "a".repeat(64), B = "b".repeat(64);
const status = {
  plan: B, previous: A, rollback: null, pending: null,
  target: { hash: A, mods: [{ id: "bigpack", version: "1.0.0" }] },
  changes: [{ field: "items[game:fixture_stack].maxStack", from: 160, to: 80 }, { field: "balance[game:balance].levelCap", from: 70, to: null }],
  changesUnknown: null, blocked: null, dryRun: true, wrote: false, cleared: null,
};

// The mock module answers from window.__plan and window.__report (the test sets them; the report as registry.json's text)
// and records every call in window.__planCalls. window.__fail: "write" fails every call that writes, "read" every dry run.
async function open(page: Page, plan: unknown, report: unknown = registryFixture()) {
  await page.addInitScript(([p, r]) => {
    const w = window as unknown as Record<string, unknown>;
    w.__plan = p; w.__planCalls = []; w.__report = JSON.stringify(r); w.__fail = null;
  }, [plan, report] as const);
  await page.route("**/src/registry/mock.ts*", route => route.fulfill({
    contentType: "application/javascript",
    body: `export async function readMockRegistryReport() { return window.__report; }
export async function mockRegistryRollback(o) {
  window.__planCalls.push(o);
  if (window.__fail === (o.dryRun ? "read" : "write")) throw new Error("kiln/data/content/registry: access denied");
  const p = window.__plan;
  if (o.clear) { window.__plan = { ...p, pending: null }; return { ...window.__plan, dryRun: false, cleared: !!p.pending }; }
  if (o.dryRun) return p;
  window.__plan = { ...p, pending: { plan: p.previous, from: p.plan, requested: "2026-11-25T20:00:00Z", by: "launcher" } };
  return { ...window.__plan, dryRun: false, wrote: true };
}`,
  }));
  await page.goto("/#/conflicts");
  await expect(page.getByRole("heading", { name: "Registry plan", exact: true })).toBeVisible();
}
const calls = (page: Page) => page.evaluate(() => (window as unknown as { __planCalls: { clear: boolean; dryRun: boolean }[] }).__planCalls);

test("the plan card names the last start's plan and the previous one", async ({ page }) => {
  await open(page, status);
  const card = page.getByTestId("registry-plan");
  await expect(card).toContainText(`Plan of the last start: ${B.slice(0, 12)}`);
  await expect(card).toContainText(`Previous (last good) plan: ${A.slice(0, 12)}`);
  await expect(card).toContainText("Rollback at the last start: none");
  await expect(card).toContainText("Rollback request: none");
  await expect(card.getByRole("button", { name: "Clear", exact: true })).toBeDisabled();
});

test("Roll back shows the dry run's changes first and writes only on confirm", async ({ page }) => {
  await open(page, status);
  const card = page.getByTestId("registry-plan");
  await card.getByRole("button", { name: "Roll back", exact: true }).click();
  await expect(card.getByText(`Rolling back to plan ${A.slice(0, 12)} changes 2 values back`, { exact: true })).toBeVisible();
  await expect(card.getByTestId("registry-plan-change")).toHaveCount(2);
  await expect(card.getByTestId("registry-plan-change").first()).toHaveText("items[game:fixture_stack].maxStack: 160 → 80");
  await expect(card.getByTestId("registry-plan-change").nth(1)).toHaveText("balance[game:balance].levelCap: 70 → no registry edit");
  await expect(card).toContainText("That plan's mods: bigpack 1.0.0");
  expect((await calls(page)).every(c => c.dryRun)).toBe(true);  // nothing written yet
  await card.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(card.getByTestId("registry-plan-change")).toHaveCount(0);
  expect((await calls(page)).every(c => c.dryRun)).toBe(true);
  await card.getByRole("button", { name: "Roll back", exact: true }).click();
  await card.getByRole("button", { name: "Confirm roll back", exact: true }).click();
  await expect(card.getByText(`Requested: the next start runs on plan ${A.slice(0, 12)}, and every start after it until you clear it.`, { exact: true })).toBeVisible();
  expect(await calls(page)).toContainEqual({ clear: false, dryRun: false });
  await expect(card).toContainText(`Rollback request: plan ${A.slice(0, 12)}, requested 2026-11-25T20:00:00Z by launcher`);
});

test("Clear asks first, then removes the request", async ({ page }) => {
  await open(page, { ...status, pending: { plan: A, from: B, requested: "2026-11-25T20:00:00Z", by: "kilnctl" } });
  const card = page.getByTestId("registry-plan");
  await card.getByRole("button", { name: "Clear", exact: true }).click();
  await expect(card.getByText(`Remove the rollback request for plan ${A.slice(0, 12)}`, { exact: true })).toBeVisible();
  await expect(card).toContainText("The next start runs on the mods' current registry edits again.");
  expect((await calls(page)).some(c => c.clear)).toBe(false);
  await card.getByRole("button", { name: "Confirm clear", exact: true }).click();
  await expect(card.getByText("Rollback request removed", { exact: true })).toBeVisible();
  expect(await calls(page)).toContainEqual({ clear: true, dryRun: false });
  await expect(card).toContainText("Rollback request: none");
});

test("a plan that cannot be rolled back says why, with Roll back off", async ({ page }) => {
  await open(page, { ...status, previous: null, target: null, changes: [], blocked: "No earlier good plan to roll back to." });
  const card = page.getByTestId("registry-plan");
  await expect(card).toContainText("Previous (last good) plan: none");
  await expect(card).toContainText("No earlier good plan to roll back to.");
  await expect(card.getByRole("button", { name: "Roll back", exact: true })).toBeDisabled();
});

test("the last start's rollback is shown with its state and reason", async ({ page }) => {
  await open(page, { ...status, rollback: { plan: A, from: B, state: "refused", reason: "bigpack is not loaded at 1.0.0" } });
  await expect(page.getByTestId("registry-plan")).toContainText(`Rollback at the last start: refused (plan ${A.slice(0, 12)}) bigpack is not loaded at 1.0.0`);
});

test("plans that cannot be read say so, with Roll back off", async ({ page }) => {
  await open(page, status);
  await page.evaluate(() => {  // a new report (the game started again): the card reads the plans again, and that fails
    const w = window as unknown as Record<string, unknown>;
    w.__fail = "read";
    w.__report = JSON.stringify({ ...JSON.parse(String(w.__report)), written: new Date(Date.now() + 60_000).toISOString() });
  });
  await page.getByRole("button", { name: "Refresh report", exact: true }).click();
  const card = page.getByTestId("registry-plan");
  await expect(card.getByText("The registry plans could not be read", { exact: true })).toBeVisible();
  await expect(card.getByRole("button", { name: "Roll back", exact: true })).toBeDisabled();
});

// The launcher stays open while the game starts again (to see whether a rollback applied): the new start's report has a
// new write time but may have the same generation (it starts again at 1 each start). The card reads the plans again.
test("a new report (the game started again) reads the plans again", async ({ page }) => {
  await open(page, status);
  const card = page.getByTestId("registry-plan");
  await expect(card).toContainText(`Plan of the last start: ${B.slice(0, 12)}`);
  const C = "c".repeat(64);
  await page.evaluate(([a, b, c]) => {
    const w = window as unknown as Record<string, unknown>;
    w.__plan = { ...(w.__plan as object), plan: a, previous: c, rollback: { plan: a, from: b, state: "applied", reason: null }, target: null, changes: [] };
    w.__report = JSON.stringify({ ...JSON.parse(String(w.__report)), written: new Date(Date.now() + 60_000).toISOString() });
  }, [A, B, C]);
  await page.getByRole("button", { name: "Refresh report", exact: true }).click();
  await expect(card).toContainText(`Plan of the last start: ${A.slice(0, 12)}`);
  await expect(card).toContainText(`Rollback at the last start: applied (plan ${A.slice(0, 12)})`);
});
