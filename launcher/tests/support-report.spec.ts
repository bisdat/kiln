// The support report block on Logs (docs/kiln/launcher/support-report.md): "Save report..." (through "What's in this
// report": tests/report-contents.spec.ts) and "Copy summary" in the browser mock (no file dialog there: the mock answers
// report_save with the default name).
import { expect, test, type Page } from "@playwright/test";

async function desktop(page: Page, route: string) {
  await page.goto(`/#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

test("Logs offers the support report: save it, copy the summary", async ({ page, context }) => {
  await context.grantPermissions(["clipboard-read", "clipboard-write"]);
  await desktop(page, "/logs");
  const block = page.getByTestId("support-report");
  await expect(block).toContainText("Support report");
  await expect(block).toContainText("paths and server addresses are replaced");
  await block.getByRole("button", { name: "Save report..." }).click();
  await page.getByTestId("krc-save").click();
  await expect(page.locator(".k-toasts")).toContainText("Report saved");
  await expect(page.locator(".k-toasts")).toContainText(/kiln-report-\d{4}-\d{2}-\d{2}\.zip/);
  await block.getByRole("button", { name: "Copy summary" }).click();
  await expect(page.locator(".k-toasts")).toContainText("Summary copied");
  const copied = await page.evaluate(() => navigator.clipboard.readText());
  expect(copied).toContain("Kiln support report");
  expect(copied).toContain("Findings:");
});

test("a server's Logs page has the block too", async ({ page }) => {
  await desktop(page, "/servers/lab/logs");
  await expect(page.getByTestId("support-report")).toBeVisible();
});

test("a desktop without Deck detection keeps the normal Save report action", async ({ page }) => {
  await desktop(page, "/logs");
  const block = page.getByTestId("support-report");
  await expect(block.getByRole("button", { name: "Save report...", exact: true })).toBeVisible();
  await expect(block.getByRole("button", { name: "Save to Downloads", exact: true })).toHaveCount(0);
});

test("a detected Steam Deck offers Save to Downloads beside Save report, without a dialog", async ({ page }) => {
  await page.goto("/?deck=steamdeck#/logs");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  const block = page.getByTestId("support-report");
  const button = block.getByRole("button", { name: "Save to Downloads", exact: true });
  await expect(button).toBeVisible();
  expect(await button.evaluate((el) => el.previousElementSibling?.textContent)).toBe("Save report...");
  await button.click();
  await expect(page.getByRole("dialog")).toHaveCount(0);
  await expect(page.locator(".k-toasts")).toContainText(/\/home\/player\/Downloads\/kiln-report-\d{4}-\d{2}-\d{2}\.zip/);
  const open = page.locator(".k-toasts").getByRole("button", { name: "Open folder" });
  await expect(open).toBeVisible();
  await open.click();
  await expect.poll(() => page.evaluate(() => (window as unknown as { __kilnMock: { openedPaths: () => string[] } }).__kilnMock.openedPaths())).toEqual(["/home/player/Downloads/"]);
});
