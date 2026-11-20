// Inbox 92 (29.18): after it blocked Kiln's install, a player's antivirus also took the launcher's exe, its uninstaller
// and its setup. A launcher that still starts now sees its own uninstall.exe (or its exe) gone from its folder
// (own_files.rs, app_info's ownFilesMissing) and says so once a start, with the docs' Antivirus section.
import { expect, test, type Page } from "@playwright/test";

type Mock = { __kilnMock: { opened: () => string[] } };

async function start(page: Page, query: string) {
  await page.goto(`/${query}#/`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

test("the launcher's own uninstall.exe an antivirus took is said once, with the antivirus docs", async ({ page }) => {
  await start(page, "?ownFiles=uninstall.exe");
  const notice = page.locator(".k-toast").filter({ hasText: "A file of the launcher is missing" });
  await expect(notice).toContainText("uninstall.exe is gone from the launcher's folder. An antivirus may have quarantined it. Restore it from the quarantine and add an exclusion, or install the launcher again.");
  // it stays until closed
  await page.waitForTimeout(7000);
  await expect(notice).toHaveCount(1);
  await notice.getByRole("button", { name: "Antivirus help" }).click();
  await expect.poll(() => page.evaluate(() => (window as unknown as Mock).__kilnMock.opened()))
    .toContain("https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/launcher/app.md#antivirus");
  await expect(notice).toHaveCount(0);
  // once a start: another page and back say nothing again
  await page.getByRole("navigation", { name: "Main" }).getByRole("link", { name: "Runtime" }).click();
  await page.getByRole("navigation", { name: "Main" }).getByRole("link", { name: "Play" }).click();
  await page.waitForTimeout(500);
  await expect(page.locator(".k-toast").filter({ hasText: "of the launcher" })).toHaveCount(0);
});

test("both files gone are named together; nothing gone, nothing said", async ({ page }) => {
  await start(page, "?ownFiles=Kiln%20Launcher.exe,uninstall.exe");
  await expect(page.locator(".k-toast").filter({ hasText: "Files of the launcher are missing" }))
    .toContainText("Kiln Launcher.exe, uninstall.exe are gone from the launcher's folder. An antivirus may have quarantined them.");
  await start(page, "");
  await page.waitForTimeout(1000);
  await expect(page.locator(".k-toast").filter({ hasText: "of the launcher" })).toHaveCount(0);
});
