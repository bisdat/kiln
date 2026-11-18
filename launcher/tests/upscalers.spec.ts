// Settings, Upscalers in mock mode (src/api/mockVendor.ts; docs/kiln/graphics.md "Upscaler files"): hidden while the
// preview is off; with it on (`?preview=upscalers`) the shipped manifest's two components can be downloaded; a build
// whose files are not pinned (`?vendor=unpinned`) says "Not available in this build" and offers no button. Then the
// licence dialog (what, size, host, where, the text in full, the privacy line), Cancel there sends nothing, "I agree,
// download" with the text's hash, progress, Cancel, a second download without the dialog, Downloaded, Remove; a
// failed download with its reason and Try again; an update, missing files, and a changed licence text that asks
// again. Screenshots go to .out/launcher/shots/ (SHOTS=0 skips them).
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";
import { join } from "node:path";

const SHOTS = join(import.meta.dirname, "..", "..", ".out", "launcher", "shots");

async function open(page: Page, query: string) {
  await page.goto(`/${query}#/settings`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}
type Call = { cmd: string; args: Record<string, unknown> };
const vmock = <T,>(page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(([f, a]) => (window as unknown as { __kilnVendorMock: Record<string, (...x: unknown[]) => unknown> }).__kilnVendorMock[f as string](...(a as unknown[])), [fn, args] as const) as Promise<T>;
const downloads = async (page: Page) => (await vmock<Call[]>(page, "calls")).filter((c) => c.cmd === "vendor_download");
const section = (page: Page) => page.getByRole("region", { name: "Upscalers" });
const row = (page: Page, name: string) => section(page).getByRole("listitem", { name });
async function shot(page: Page, name: string) {
  if (process.env.SHOTS === "0") return;
  mkdirSync(SHOTS, { recursive: true });
  await page.screenshot({ path: join(SHOTS, `upscalers-${name}.png`) });
}

test("no Upscalers section while the preview is off", async ({ page }) => {
  await open(page, "");
  await expect(page.getByRole("region", { name: "Graphics" })).toBeVisible();
  await expect(section(page)).toHaveCount(0);
  expect(await vmock<Call[]>(page, "calls")).toEqual([]);
});

test("with the preview on, the shipped components can be downloaded", async ({ page }) => {
  await open(page, "?preview=upscalers");
  const s = section(page);
  await expect(s).toBeVisible();
  await expect(row(page, "AMD FSR")).toContainText("Not downloaded");
  await expect(row(page, "AMD FSR").getByTestId("upscaler-detail")).toHaveText("70 MB from raw.githubusercontent.com.");
  await expect(row(page, "Intel XeSS").getByTestId("upscaler-detail")).toHaveText("74 MB from raw.githubusercontent.com.");
  await expect(s.getByRole("button", { name: /^Download (AMD FSR|Intel XeSS)$/ })).toHaveCount(2);
  // nothing is asked of a vendor until a click
  expect(await downloads(page)).toEqual([]);
  await s.scrollIntoViewIfNeeded();
  await shot(page, "section");
});

test("a build whose files are not pinned offers nothing to download", async ({ page }) => {
  await open(page, "?preview=upscalers&vendor=unpinned");
  const s = section(page);
  await expect(s).toBeVisible();
  await expect(s.getByText("Preview", { exact: true })).toBeVisible();
  await expect(s.getByTestId("upscalers-purpose")).toHaveText(
    "Kiln can run these behind the game's FSR 3 option. That arrives in a later update: until then, downloading them changes nothing in the game.");
  for (const name of ["AMD FSR", "Intel XeSS"]) {
    const r = row(page, name);
    await expect(r).toContainText("Not available in this build");
    await expect(r.getByTestId("upscaler-detail")).toHaveText("This build of Kiln does not have the files' checksums yet, so it does not download them.");
    await expect(r.getByRole("button")).toHaveCount(0);
  }
  await expect(s).not.toContainText(/dlss/i);
  await s.scrollIntoViewIfNeeded();
  await shot(page, "unavailable");
});

test("download with the licence dialog, progress and cancel, then Downloaded and Remove", async ({ page }) => {
  await open(page, "?preview=upscalers");
  await vmock(page, "pace", 250);
  const amd = row(page, "AMD FSR");
  await expect(amd).toContainText("Not downloaded");
  await expect(amd.getByTestId("upscaler-detail")).toHaveText("70 MB from raw.githubusercontent.com.");
  await expect(amd).toContainText("AMD's FSR upscalers (FSR 4 on recent AMD cards, FSR 3.1 on other cards) and FSR frame generation");

  // the dialog says what will happen; Cancel sends nothing
  await amd.getByRole("button", { name: "Download AMD FSR" }).click();
  const dialog = page.getByRole("dialog", { name: "Download AMD FSR" });
  await expect(dialog).toBeVisible();
  await expect(dialog).toContainText("70 MB, 3 files");
  await expect(dialog).toContainText("raw.githubusercontent.com: AMD's own repository on GitHub, version 2.4.0");
  await expect(dialog).toContainText("kiln/vendor/amd/2.4.0 in the game folder");
  await expect(dialog).toContainText("AMD licenses these files under the MIT licence.");
  await expect(dialog.getByTestId("upscaler-license")).toContainText("Permission is hereby granted, free of charge");
  await expect(dialog.getByTestId("upscaler-license")).toContainText("Advanced Micro Devices, Inc. All rights reserved.");
  await expect(dialog.getByTestId("upscaler-privacy")).toHaveText(
    "Kiln asks raw.githubusercontent.com for these files and sends nothing else. raw.githubusercontent.com sees your IP address and a user agent that names Kiln and its version.");
  await expect(dialog).not.toContainText(/dlss/i);
  await shot(page, "licence-dialog");
  await dialog.getByRole("button", { name: "Cancel" }).click();
  await expect(dialog).toHaveCount(0);
  expect(await downloads(page)).toEqual([]);

  // agree: the download starts with the hash of the text shown; it can be canceled
  await amd.getByRole("button", { name: "Download AMD FSR" }).click();
  await dialog.getByRole("button", { name: "I agree, download" }).click();
  await expect(dialog).toHaveCount(0);
  await expect(amd).toContainText("Downloading");
  await expect(amd.getByRole("progressbar")).toBeVisible();
  await expect(amd.getByTestId("upscaler-detail")).toContainText("Downloading amd_fidelityfx_");
  const first = await downloads(page);
  expect(first).toHaveLength(1);
  expect(first[0].args.licenseSha256).toMatch(/^[0-9a-f]{64}$/);
  expect(await vmock<string[]>(page, "agreed")).toEqual([`amd:${first[0].args.licenseSha256}`]);
  await shot(page, "downloading");
  await amd.getByRole("button", { name: "Cancel the download of AMD FSR" }).click();
  await expect(amd).toContainText("Not downloaded");
  await expect(amd.getByRole("button", { name: "Download AMD FSR" })).toBeVisible();

  // agreed already: the row starts it at once, without the dialog
  await vmock(page, "pace", 60);
  await amd.getByRole("button", { name: "Download AMD FSR" }).click();
  await expect(page.getByRole("dialog")).toHaveCount(0);
  await expect(amd).toContainText("Downloaded", { timeout: 10_000 });
  await expect(amd.getByTestId("upscaler-detail")).toHaveText("Version 2.4.0, 70 MB on disk.");
  await expect(page.locator(".k-toast", { hasText: "AMD FSR is downloaded" })).toBeVisible();
  const second = await downloads(page);
  expect(second[1].args.licenseSha256).toBeNull();
  // Intel's licence is its own: its first download still asks
  await row(page, "Intel XeSS").getByRole("button", { name: "Download Intel XeSS" }).click();
  const intel = page.getByRole("dialog", { name: "Download Intel XeSS" });
  await expect(intel).toContainText("Intel licenses these files under the Intel Simplified Software License (October 2022).");
  await expect(intel.getByTestId("upscaler-license")).toContainText("Intel Simplified Software License (Version October 2022)");
  await intel.getByRole("button", { name: "Cancel" }).click();
  await amd.scrollIntoViewIfNeeded();
  await shot(page, "downloaded");

  // Remove asks, then the row is back to not downloaded
  await amd.getByRole("button", { name: "Remove AMD FSR" }).click();
  const confirm = page.getByRole("dialog", { name: "Remove AMD FSR?" });
  await expect(confirm).toContainText("Kiln deletes kiln/vendor/amd/2.4.0 (70 MB) from the game folder.");
  await confirm.getByRole("button", { name: "Remove" }).click();
  await expect(amd).toContainText("Not downloaded");
  await expect(page.locator(".k-toast", { hasText: "AMD FSR is removed" })).toBeVisible();
});

test("a failed download says why, and Try again downloads it", async ({ page }) => {
  await open(page, "?preview=upscalers");
  await vmock(page, "pace", 40);
  const why = "libxess.dll from Intel (raw.githubusercontent.com) is not the file Kiln expects: its size or SHA-256 differs from the one this build of Kiln pinned. Kiln deleted it.";
  await vmock(page, "failNext", why);
  const intel = row(page, "Intel XeSS");
  await intel.getByRole("button", { name: "Download Intel XeSS" }).click();
  await page.getByRole("dialog", { name: "Download Intel XeSS" }).getByRole("button", { name: "I agree, download" }).click();
  await expect(intel).toContainText("Failed", { timeout: 10_000 });
  await expect(intel.getByTestId("upscaler-reason")).toHaveText(why);
  await intel.scrollIntoViewIfNeeded();
  await shot(page, "failed");
  await intel.getByRole("button", { name: "Try the download of Intel XeSS again" }).click();
  await expect(intel).toContainText("Downloaded", { timeout: 10_000 });
  await expect(intel.getByTestId("upscaler-reason")).toHaveCount(0);
  await expect(intel.getByTestId("upscaler-detail")).toHaveText("Version 3.0.2, 74 MB on disk.");
});

/** Leaves Settings and comes back: the section asks for the status again (as after the player changed the folder). */
async function again(page: Page) {
  await page.evaluate(() => { location.hash = "#/about"; });
  await expect(section(page)).toHaveCount(0);
  await page.evaluate(() => { location.hash = "#/settings"; });
  await expect(section(page)).toBeVisible();
}

test("an update, missing files, and a changed licence text that asks again", async ({ page }) => {
  await open(page, "?preview=upscalers");
  await vmock(page, "pace", 40);
  await vmock(page, "install", "amd-fsr", "2.3.0");
  await again(page);
  const amd = row(page, "AMD FSR");
  await expect(amd).toContainText("Update available");
  await expect(amd.getByTestId("upscaler-detail")).toHaveText("Version 2.3.0 is downloaded. Version 2.4.0 is out: 70 MB from raw.githubusercontent.com.");
  await amd.getByRole("button", { name: "Update AMD FSR" }).click();
  await page.getByRole("dialog", { name: "Download AMD FSR" }).getByRole("button", { name: "I agree, download" }).click();
  await expect(amd).toContainText("Downloaded", { timeout: 10_000 });
  await expect(amd.getByTestId("upscaler-detail")).toHaveText("Version 2.4.0, 70 MB on disk.");

  // a file gone: failed with the reason, Download again (agreed already: no dialog)
  await vmock(page, "damage", "amd-fsr", "amd_fidelityfx_upscaler_vk.dll");
  await again(page);
  await expect(amd).toContainText("Failed");
  await expect(amd.getByTestId("upscaler-reason")).toHaveText("amd_fidelityfx_upscaler_vk.dll of it is missing or changed. Download it again.");
  await amd.getByRole("button", { name: "Download AMD FSR again" }).click();
  await expect(page.getByRole("dialog")).toHaveCount(0);
  await expect(amd).toContainText("Downloaded", { timeout: 10_000 });

  // AMD changes the text of its licence: the next download shows the dialog again
  await amd.getByRole("button", { name: "Remove AMD FSR" }).click();
  await page.getByRole("dialog", { name: "Remove AMD FSR?" }).getByRole("button", { name: "Remove" }).click();
  await expect(amd).toContainText("Not downloaded");
  await vmock(page, "changeLicense", "amd-fsr");
  await again(page);
  await amd.getByRole("button", { name: "Download AMD FSR" }).click();
  await expect(page.getByRole("dialog", { name: "Download AMD FSR" })).toBeVisible();
});
