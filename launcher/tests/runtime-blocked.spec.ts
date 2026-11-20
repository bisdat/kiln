// Inbox 92 (29.18): an antivirus held kiln-greeneye.dll while Kiln installed it, and the install failed with Windows' raw
// "Access is denied. (os error 5)" and nothing else. The backend (kiln_manage::blocked) now names a DLL or exe Windows
// refused in the error's detail.blocked. Runtime's and Play's banners and the toast say in plain words that an antivirus
// or another program may be blocking it, name it and link the docs' Antivirus section; the raw error stays under it.
// Any other error is said as before.
import { expect, test, type Page } from "@playwright/test";

type Mock = { __kilnMock: { failNextInstall: (f: unknown) => void; setMissing: (files: string[]) => void; opened: () => string[] } };

const RAW = "installing C:\\Games\\Enshrouded\\kiln/plugins/kiln-greeneye.dll: Access is denied. (os error 5)";
const SAID = "Windows did not let Kiln write kiln/plugins/kiln-greeneye.dll. An antivirus or another program may be blocking it.";
const INSTALLED = "0.2.0-dev+20261104.0f1ffa6";

async function runtimePage(page: Page) {
  await page.goto("/#/runtime");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  if (await page.evaluate(() => location.hash !== "#/runtime")) await page.evaluate(() => { location.hash = "#/runtime"; });
  await expect(page.getByRole("row").filter({ hasText: INSTALLED })).toBeVisible();
}

const banner = (page: Page, title: string) => page.locator(".k-banner").filter({ has: page.locator(".k-banner-title", { hasText: title }) });

/** Repair of a DLL the antivirus took (inbox 85's banner), failing with `failure`. */
async function repairFails(page: Page, failure: unknown) {
  await page.evaluate(() => (window as unknown as Mock).__kilnMock.setMissing(["kiln/plugins/kiln-greeneye.dll"]));
  await page.evaluate((f) => (window as unknown as Mock).__kilnMock.failNextInstall(f), failure);
  await banner(page, "Files missing").getByRole("button", { name: "Repair" }).click();
  await page.getByRole("dialog", { name: "Install Kiln 0.2.0-dev" }).getByRole("button", { name: "Install" }).click();
}

test("a DLL Windows refused: the banners say an antivirus may be blocking it, name it, link the docs and keep the raw error", async ({ page }) => {
  await runtimePage(page);
  await repairFails(page, { code: "permission", message: RAW, detail: { blocked: { path: "kiln/plugins/kiln-greeneye.dll", osError: 5 } } });
  const failed = banner(page, `Update to ${INSTALLED} failed`);
  await expect(failed.getByTestId("runtime-blocked")).toHaveText(SAID, { timeout: 8000 });
  await expect(failed).toContainText(`Add an exclusion for the game folder in your antivirus, then try again. The game keeps Kiln ${INSTALLED}.`);
  await expect(failed.getByTestId("runtime-blocked-raw")).toHaveText(`Details: ${RAW}`);
  // the toast: the plain words as its title, the raw error under them
  const toast = page.locator(".k-toast-err").filter({ hasText: SAID });
  await expect(toast).toContainText(RAW);
  await failed.getByRole("button", { name: "Antivirus help" }).click();
  await expect.poll(() => page.evaluate(() => (window as unknown as Mock).__kilnMock.opened()))
    .toContain("https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/launcher/app.md#antivirus");
  // Play says it too, until dismissed
  await page.getByRole("navigation", { name: "Main" }).getByRole("link", { name: "Play" }).click();
  const onPlay = page.locator(".k-hero-banner .k-banner").filter({ hasText: `Update to ${INSTALLED} failed` });
  await expect(onPlay.getByTestId("runtime-blocked")).toHaveText(SAID);
  await expect(onPlay).toContainText(`The game still has Kiln ${INSTALLED}.`);
  await expect(onPlay.getByTestId("runtime-blocked-raw")).toHaveText(`Details: ${RAW}`);
  await expect(onPlay.getByRole("button", { name: "Antivirus help" })).toBeVisible();
  await onPlay.getByRole("button", { name: "Dismiss" }).click();
  await expect(onPlay).toHaveCount(0);
});

test("a refusal without detail.blocked (Linux, another file, an older backend) is said as before, with no antivirus", async ({ page }) => {
  await runtimePage(page);
  const raw = "installing /games/Enshrouded/kiln/plugins/kiln-greeneye.dll: Permission denied (os error 13)";
  await repairFails(page, { code: "permission", message: raw });
  const failed = banner(page, `Update to ${INSTALLED} failed`);
  await expect(failed).toContainText(`${raw} The game keeps Kiln ${INSTALLED}.`, { timeout: 8000 });
  await expect(failed).not.toContainText("antivirus");
  await expect(failed.getByTestId("runtime-blocked")).toHaveCount(0);
  await expect(failed.getByRole("button", { name: "Antivirus help" })).toHaveCount(0);
  await expect(page.locator(".k-toast-err").filter({ hasText: raw })).toBeVisible();
});
