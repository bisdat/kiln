import { expect, test, type Page } from "@playwright/test";
import { emptyRegistryFixture, registryFixture } from "./fixtures/registryReport";

async function report(page: Page, value: unknown, error = false) {
  await page.route("**/src/registry/mock.ts*", route => route.fulfill({
    contentType: "application/javascript",
    body: `export async function readMockRegistryReport() { ${error ? 'throw new Error("fixture: report unreadable")' : `return ${JSON.stringify(value === null ? null : typeof value === "string" ? value : JSON.stringify(value))}`}; }
export async function mockRegistryRollback(o) { return { plan: null, previous: null, rollback: null, pending: null, target: null, changes: [], changesUnknown: null, blocked: "fixture: no plans", dryRun: o.dryRun, wrote: false, cleared: o.clear ? false : null }; }`,
  }));
  await page.goto("/#/conflicts");
  await expect(page.getByRole("heading", { name: "Registry conflicts", exact: true })).toBeVisible();
}

test("missing report is explicit and reachable from launcher navigation", async ({ page }) => {
  await report(page, null);
  await expect(page.getByText("No registry report yet", { exact: true })).toBeVisible();
  await page.getByRole("link", { name: "Conflicts", exact: true }).click();
  await expect(page).toHaveURL(/#\/conflicts$/);
});

test("shows base, ordered owners and operations, winner, certainty and refusal reasons", async ({ page }) => {
  await report(page, registryFixture());
  await expect(page.getByRole("heading", { name: "Refused edits" })).toBeVisible();
  // the RestartRequired row is in the refused list, and (since 0.2.0-dev.31.14) in the restart card above it too
  await expect(page.locator(".registry-line").filter({ hasText: /RestartRequired: balance.levelCap/ })).toHaveCount(2);
  await expect(page.getByText("Unproven: items.maxStack (no verified mapping for this build)", { exact: true })).toBeVisible();
  await expect(page.getByText("Best-effort drift", { exact: true }).first()).toBeVisible();
  await expect(page.getByText("Certain", { exact: true }).first()).toBeVisible();
  const field = page.locator(".registry-field").first();
  await field.locator("summary").click();
  await expect(field).toContainText("Base origin: archive");
  await expect(field).toContainText('"mul":2');
  await expect(field).toContainText("Winner: stacker");
  const owners = field.locator("ol li");
  await expect(owners.nth(0)).toContainText("earlier");
  await expect(owners.nth(1)).toContainText("stacker");
  await page.getByRole("button", { name: "Refresh report" }).click();
  await expect(page.getByRole("heading", { name: "Refused edits" })).toBeVisible();
});

test("edits that wait for a restart come first, with their count, mod and field (0.2.0-dev.31.14)", async ({ page }) => {
  await report(page, { ...registryFixture(), counts: { ...registryFixture().counts, restartRequired: 3 } });
  await expect(page.getByRole("heading", { name: "Waits for a restart" })).toBeVisible();
  await expect(page.getByText("3 registry edits wait for a restart", { exact: true })).toBeVisible();
  await expect(page.getByText("2 more are not shown here: state/registry.json lists every one.", { exact: true })).toBeVisible();
  const row = page.getByTestId("registry-restart-row");
  await expect(row).toHaveCount(1);
  await expect(row).toContainText("late");
  await expect(row).toContainText("balance · levelCap");
  await expect(row).toContainText("RestartRequired: balance.levelCap is read at game start");
});

test("without edits that wait for a restart there is no restart card", async ({ page }) => {
  await report(page, { ...registryFixture(), refused: [registryFixture().refused[1]], counts: { ...registryFixture().counts, refused: 1, restartRequired: 0 } });
  await expect(page.getByRole("heading", { name: "Refused edits" })).toBeVisible();
  await expect(page.getByRole("heading", { name: "Waits for a restart" })).toHaveCount(0);
});

test("empty snapshot is different from a missing report", async ({ page }) => {
  await report(page, emptyRegistryFixture());
  await expect(page.getByText("No registry edits in this snapshot", { exact: true })).toBeVisible();
  await expect(page.getByText("Base origin: live", { exact: true })).toBeVisible();
});

test("previous snapshot remains readable but is clearly stale", async ({ page }) => {
  await report(page, { ...registryFixture(), written: "2020-01-01T00:00:00Z" });
  await expect(page.getByText("Stale snapshot", { exact: true })).toBeVisible();
  await expect(page.locator(".registry-field")).toHaveCount(1);
});

test("read errors have an explicit retryable state", async ({ page }) => {
  await report(page, null, true);
  await expect(page.getByText("Registry report unavailable", { exact: true })).toBeVisible();
  await expect(page.getByRole("button", { name: "Refresh report" })).toBeEnabled();
});

test("malformed reports cannot crash the page", async ({ page }) => {
  await report(page, '{"format":1,"fields":[null]}');
  await expect(page.getByText("Registry report unavailable", { exact: true })).toBeVisible();
  await expect(page.getByRole("link", { name: "Play", exact: true })).toBeVisible();
});

test("hostile text is escaped and raw paths and addresses stay hidden", async ({ page }) => {
  const r = registryFixture();
  r.refused[0].reason = '<img src=x onerror="window.registryInjected=true">';
  await report(page, { ...r, fields: [{ ...r.fields[0], raw: true, field: "SecretSetup.Offset" }], regmap: { ...r.regmap, record: "0xDEADBEEF" } });
  await expect(page.getByText(r.refused[0].reason, { exact: true })).toBeVisible();
  expect(await page.evaluate(() => (window as Window & { registryInjected?: boolean }).registryInjected)).toBeUndefined();
  await expect(page.locator(".registry-conflicts img")).toHaveCount(0);
  await expect(page.locator(".registry-conflicts")).not.toContainText("SecretSetup");
  await expect(page.locator(".registry-conflicts")).not.toContainText("DEADBEEF");
});

test("huge report is rejected without hanging navigation", async ({ page }) => {
  await report(page, " ".repeat(8 * 1024 * 1024 + 1));
  await expect(page.getByText("Registry report unavailable", { exact: true })).toBeVisible();
  await expect(page.getByText(/8 MiB/)).toBeVisible();
  await page.getByRole("link", { name: "Play", exact: true }).click();
  await expect(page).toHaveURL(/#\/$/);
});

test("a list-element writer shows the element it changed", async ({ page }) => {
  const r = registryFixture();
  const row = { ...r.fields[0], registry: "drops", id: "game:fixture_drops", field: "entries",
    writers: [{ kind: "registry", mod: "loot", stage: "alter", n: 0, element: "entries[3].chance", op: { set: 0.5 }, value: 0.5, certainty: "certain" }] };
  await report(page, { ...r, fields: [row] });
  const field = page.locator(".registry-field").first();
  await field.locator("summary").click();
  await expect(field).toContainText("Element: entries[3].chance");
});

test("writer-only truncation still marks the report partial", async ({ page }) => {
  const r = registryFixture();
  await report(page, { ...r, fields: [{ ...r.fields[0], writersLeftOut: 3 }], truncated: { ...r.truncated, writers: 3 } });
  await expect(page.getByText("Partial report", { exact: true })).toBeVisible();
  await expect(page.getByText(/3 earlier writers were left out of the writer chains/)).toBeVisible();
  await expect(page.locator(".registry-field summary")).toContainText("3 earlier writers omitted");
});

test("long field lists paginate, filter and disclose native omissions", async ({ page }) => {
  const r = registryFixture();
  await report(page, { ...r, fields: Array.from({ length: 30 }, (_, i) => ({ ...r.fields[0], id: `game:fixture_${i}` })), truncated: { ...r.truncated, fields: 20 } });
  await expect(page.getByText("Partial report", { exact: true })).toBeVisible();
  await expect(page.locator(".registry-field")).toHaveCount(25);
  await page.getByRole("button", { name: "Next", exact: true }).click();
  await expect(page.locator(".registry-field")).toHaveCount(5);
  await page.getByRole("textbox", { name: "Filter fields or owners" }).fill("fixture_29");
  await expect(page.locator(".registry-field")).toHaveCount(1);
  await expect(page.locator(".registry-field")).toContainText("game:fixture_29");
  await page.getByRole("textbox", { name: "Filter fields or owners" }).fill("not-present");
  await expect(page.getByText("No displayed fields match this filter.", { exact: true })).toBeVisible();
});
