// Inbox 85 (29.18): an antivirus deleted <game>\kiln\plugins\kiln-greeneye.dll. Runtime counted it as "Changed" and
// offered no Reinstall (only missing parts had one). A missing receipt file now has its own banner that names it, says
// an antivirus may have removed a DLL with a link to the docs, and offers Repair (the installed bundle again); a file
// that is there with other bytes keeps the Changed banner, without a button.
import { expect, test, type Page } from "@playwright/test";

type Mock = { __kilnMock: { setMissing: (files: string[]) => void; setModified: (files: string[]) => void; opened: () => string[] } };

async function runtimePage(page: Page) {
  await page.goto("/#/runtime");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  if (await page.evaluate(() => location.hash !== "#/runtime")) await page.evaluate(() => { location.hash = "#/runtime"; });
  await expect(page.getByRole("row").filter({ hasText: "0.2.0-dev+20261104.0f1ffa6" })).toBeVisible();
}

const banner = (page: Page, title: string) => page.locator(".k-banner").filter({ has: page.locator(".k-banner-title", { hasText: title }) });

test("a DLL an antivirus removed: Runtime names it, links the antivirus docs and Repair puts it back", async ({ page }) => {
  await runtimePage(page);
  await page.evaluate(() => (window as unknown as Mock).__kilnMock.setMissing(["kiln/plugins/kiln-greeneye.dll"]));
  const missing = banner(page, "Files missing");
  await expect(missing.getByTestId("runtime-missing-files")).toHaveText("1 of Kiln's files is missing: kiln/plugins/kiln-greeneye.dll");
  await expect(missing).toContainText("An antivirus may have removed it. Repair puts it back.");
  // a missing file is not also a changed one
  await expect(banner(page, "Changed")).toHaveCount(0);
  await missing.getByRole("button", { name: "Antivirus help" }).click();
  await expect.poll(() => page.evaluate(() => (window as unknown as Mock).__kilnMock.opened()))
    .toContain("https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/launcher/app.md#antivirus");
  // the installed bundle's row offers it too
  const row = page.getByRole("row").filter({ hasText: "0.2.0-dev+20261104.0f1ffa6" });
  await expect(row.getByRole("button", { name: "Reinstall" })).toBeEnabled();
  await missing.getByRole("button", { name: "Repair" }).click();
  const dialog = page.getByRole("dialog", { name: "Install Kiln 0.2.0-dev" });
  await dialog.getByRole("button", { name: "Install" }).click();
  await expect(page.locator(".k-toast").filter({ hasText: "Kiln 0.2.0-dev+20261104.0f1ffa6 installed" })).toBeVisible({ timeout: 8000 });
  await expect(missing).toHaveCount(0);
  await expect(banner(page, "Changed")).toHaveCount(0);
});

test("a changed file keeps its banner without Repair; a missing file that is no DLL names no antivirus", async ({ page }) => {
  await runtimePage(page);
  await page.evaluate(() => {
    const mock = (window as unknown as Mock).__kilnMock;
    mock.setModified(["kiln/kiln-core.dll"]);
    mock.setMissing(["kiln/sigdb/client.json", "kiln/host/Kiln.Host.deps.json"]);
  });
  const missing = banner(page, "Files missing");
  await expect(missing.getByTestId("runtime-missing-files")).toHaveText("2 of Kiln's files are missing: kiln/sigdb/client.json, kiln/host/Kiln.Host.deps.json");
  await expect(missing).toContainText("Repair puts them back.");
  await expect(missing).not.toContainText("antivirus");
  await expect(missing.getByRole("button", { name: "Antivirus help" })).toHaveCount(0);
  const changed = banner(page, "Changed");
  await expect(changed).toContainText("1 file changed since the install: kiln/kiln-core.dll");
  await expect(changed.getByRole("button")).toHaveCount(0);
});

test("a changed file alone offers no Repair, as before", async ({ page }) => {
  await runtimePage(page);
  await page.evaluate(() => (window as unknown as Mock).__kilnMock.setModified(["kiln/kiln-core.dll"]));
  await expect(banner(page, "Changed")).toContainText("1 file changed since the install: kiln/kiln-core.dll");
  await expect(banner(page, "Files missing")).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Repair" })).toHaveCount(0);
});
