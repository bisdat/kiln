// (dev.29.18) The upscaler behind the game's FSR 3 option in mock mode (docs/kiln/graphics.md "Behind the game's FSR 3"):
// on the desktop's Settings, Graphics card (choose, the offer of the files with the licence first, progress, the plan,
// Remove, "Now" while the game runs and "Last game" after, a tool Kiln steps aside for) and on the Deck's Settings (the
// same choice, the licence screen scrolled by the right stick, progress, Remove, the "Upscaler now" row).
// Screenshots go to .out/launcher/shots/ (SHOTS=0 skips them).
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";
import { join } from "node:path";

const SHOTS = join(import.meta.dirname, "..", "..", ".out", "launcher", "shots");
async function shot(page: Page, name: string) {
  if (process.env.SHOTS === "0") return;
  mkdirSync(SHOTS, { recursive: true });
  await page.screenshot({ path: join(SHOTS, `upscaler-${name}.png`) });
}
type Mock = Record<string, (...x: unknown[]) => unknown>;
const mock = <T,>(page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(([f, a]) => (window as unknown as { __kilnMock: Mock }).__kilnMock[f as string](...(a as unknown[])), [fn, args] as const) as Promise<T>;
const vmock = <T,>(page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(([f, a]) => (window as unknown as { __kilnVendorMock: Mock }).__kilnVendorMock[f as string](...(a as unknown[])), [fn, args] as const) as Promise<T>;
const kilnJson = async (page: Page) => (await mock<{ graphics?: Record<string, unknown> }>(page, "kilnJsonValues")).graphics ?? {};

async function open(page: Page) {
  await page.goto("/#/settings");
  await page.locator(".k-splash").waitFor({ state: "detached" });
}
/** Leaves the page and comes back (the mock keeps its state; a reload would forget it): the page reads again. */
async function again(page: Page, away: string, back: string) {
  await page.evaluate((h) => { location.hash = h; }, away);
  await page.waitForTimeout(150);
  await page.evaluate((h) => { location.hash = h; }, back);
}
const block = (page: Page) => page.getByRole("group", { name: "Behind the game's FSR 3" });
const files = (page: Page) => page.getByRole("region", { name: "Upscaler files" });

const state = (upscaler: Record<string, unknown>) => ({
  format: 1, pid: 4242, written: "2026-11-18T22:10:04Z", kiln: "0.2.0-dev.29.18",
  upscaler: { chosen: "amd", amdUpscaler: "auto", running: "amdFsr4", reason: null, hooked: true, fsrSeen: true, filesOk: true, deviceOk: true, aside: [], ...upscaler },
  files: { amd: { version: "2.4.0", state: "present" }, intel: { version: "3.0.2", state: "missing" } },
});

test.describe("desktop: Settings, Graphics", () => {
  test("choose AMD FSR: kiln.json, the offer with the licence first, progress, the plan, Remove", async ({ page }) => {
    await open(page);
    await vmock(page, "pace", 120);
    const b = block(page);
    await expect(b).toBeVisible();
    // the game's own by default: nothing offered, nothing said but the hint
    await expect(b.getByLabel("Upscaler")).toHaveValue("game");
    await expect(b.getByTestId("upscaler-offer")).toHaveCount(0);
    await expect(b.getByTestId("upscaler-status")).toHaveCount(0);
    await expect(b).toContainText("Choose FSR 3 as the upscaler in the game's graphics settings");
    // the upscaler's own words never say DLSS or unlock
    await expect(b).not.toContainText(/dlss|unlock/i);
    await expect(files(page)).not.toContainText(/dlss|unlock/i);

    await b.getByLabel("Upscaler").selectOption("amd");
    await expect.poll(async () => (await kilnJson(page)).upscaler).toBe("amd");
    await expect(b.getByLabel("AMD FSR version")).toHaveValue("auto");
    await expect(b).toContainText("FSR 4 on graphics cards and drivers that run it (AMD's own selection), FSR 3.1 on the others.");
    await expect(b.getByTestId("upscaler-offer")).toHaveText("AMD FSR's files are not in this game yet. Until they are, the game's own FSR 3 runs.");
    await expect(b.getByTestId("upscaler-status")).toHaveAttribute("data-heading", "plan");
    await expect(b.getByTestId("upscaler-status")).toContainText("When you play");
    await expect(b.getByTestId("upscaler-status-line")).toHaveText(["AMD FSR's files are not downloaded yet, so the game's own FSR 3 runs."]);
    await b.scrollIntoViewIfNeeded();
    await shot(page, "offer");

    // the offer's Download: the licence first
    await b.getByRole("button", { name: "Download AMD FSR" }).click();
    const dialog = page.getByRole("dialog", { name: "Download AMD FSR" });
    await expect(dialog).toBeVisible();
    await expect(dialog.getByTestId("upscaler-license")).toContainText("Permission is hereby granted, free of charge");
    expect((await vmock<{ cmd: string }[]>(page, "calls")).filter((c) => c.cmd === "vendor_download")).toEqual([]);
    await dialog.getByRole("button", { name: "I agree, download" }).click();
    // progress under the choice and in the list, Cancel in both
    await expect(b.getByRole("progressbar")).toBeVisible();
    await expect(b.getByRole("button", { name: "Cancel the download of AMD FSR" })).toBeVisible();
    await expect(files(page).getByRole("listitem", { name: "AMD FSR" })).toContainText("Downloading");
    await shot(page, "offer-downloading");
    await expect(files(page).getByRole("listitem", { name: "AMD FSR" })).toContainText("Downloaded", { timeout: 15_000 });
    // the files are there: no offer, the plan says what will run
    await expect(b.getByTestId("upscaler-offer")).toHaveCount(0);
    await expect(b.getByTestId("upscaler-status-line")).toHaveText([
      "With FSR 3 chosen in the game's graphics settings, Kiln runs AMD FSR in its place: FSR 4 where your graphics card and driver run it, else FSR 3.1."]);
    await b.getByLabel("AMD FSR version").selectOption("fsr3");
    await expect.poll(async () => (await kilnJson(page)).amdUpscaler).toBe("fsr3");
    await expect(b.getByTestId("upscaler-status-line")).toHaveText(["With FSR 3 chosen in the game's graphics settings, Kiln runs AMD FSR 3.1 in its place."]);
    await shot(page, "plan");

    // Intel XeSS: its own licence, its own offer; the AMD version is gone
    await b.getByLabel("Upscaler").selectOption("xess");
    await expect.poll(async () => (await kilnJson(page)).upscaler).toBe("xess");
    await expect(b.getByLabel("AMD FSR version")).toHaveCount(0);
    await expect(b.getByTestId("upscaler-offer")).toHaveText("Intel XeSS's files are not in this game yet. Until they are, the game's own FSR 3 runs.");
    await b.getByRole("button", { name: "Download Intel XeSS" }).click();
    await expect(page.getByRole("dialog", { name: "Download Intel XeSS" })).toContainText("Intel Simplified Software License");
    await page.getByRole("dialog", { name: "Download Intel XeSS" }).getByRole("button", { name: "Cancel" }).click();

    // Remove AMD FSR from the list, back to AMD: the offer is back
    await files(page).getByRole("button", { name: "Remove AMD FSR" }).click();
    await page.getByRole("dialog", { name: "Remove AMD FSR?" }).getByRole("button", { name: "Remove" }).click();
    await expect(files(page).getByRole("listitem", { name: "AMD FSR" })).toContainText("Not downloaded");
    await b.getByLabel("Upscaler").selectOption("amd");
    await expect(b.getByTestId("upscaler-offer")).toBeVisible();
    // agreed already: the offer's Download starts at once
    await b.getByRole("button", { name: "Download AMD FSR" }).click();
    await expect(page.getByRole("dialog")).toHaveCount(0);
    await expect(files(page).getByRole("listitem", { name: "AMD FSR" })).toContainText("Downloaded", { timeout: 15_000 });
  });

  test("Now while the game runs, Last game after, with the reason and the game's detail", async ({ page }) => {
    await open(page);
    await mock(page, "setKilnJson", "graphics", { upscaler: "amd", amdUpscaler: "auto" });
    await vmock(page, "install", "amd-fsr", "2.4.0");
    await again(page, "#/about", "#/settings");
    const b = block(page);
    await expect(b.getByLabel("Upscaler")).toHaveValue("amd");
    await mock(page, "setGraphicsState", state({ running: "amdFsr4", version: "4.1.2" }));
    await mock(page, "startOutside", 5);
    await expect(b.getByTestId("upscaler-status")).toHaveAttribute("data-heading", "now");
    // the files' rows wait for the game to close too (vendor_download and vendor_remove refuse while it runs), and say why
    const amdRow = files(page).getByRole("listitem", { name: "AMD FSR" });
    await expect(amdRow.getByRole("button", { name: "Remove AMD FSR" })).toBeDisabled();
    await expect(amdRow.getByTestId("upscaler-wait")).toHaveText("Close the game to download or remove these files: the launcher does not change the game's files while it runs.");
    await expect(files(page).getByRole("listitem", { name: "Intel XeSS" }).getByRole("button", { name: "Download Intel XeSS" })).toBeDisabled();
    // Intel XeSS chosen while the game runs: its files cannot come now, and the offer says so
    await b.getByLabel("Upscaler").selectOption("xess");
    await expect(b.getByTestId("upscaler-offer-running")).toHaveText("Close the game to download them: the launcher does not change the game's files while it runs.");
    await expect(b.getByRole("button", { name: "Download Intel XeSS" })).toBeDisabled();
    await expect(b.getByTestId("upscaler-status-line").last()).toHaveText("You have chosen Intel XeSS since that game started: it applies when the game next sets FSR up, or from its next start.");
    await b.getByLabel("Upscaler").selectOption("amd");
    await expect(b.getByTestId("upscaler-offer")).toHaveCount(0);
    await expect(b.getByTestId("upscaler-status")).toContainText("Now");
    await expect(b.getByTestId("upscaler-status-line")).toHaveText(["AMD FSR 4 runs in place of the game's FSR 3.", "Version 4.1.2."]);
    await b.scrollIntoViewIfNeeded();
    await shot(page, "now");
    // the game falls back: the reason in the shared words, the detail muted
    await mock(page, "setGraphicsState", state({ running: "gameFsr3", reason: "device", filesOk: true, deviceOk: false, detail: "VK_KHR_shader_float16_int8 is missing" }));
    await expect(b.getByTestId("upscaler-status-line")).toHaveText(["The game's own FSR 3 runs.",
      "Your graphics card or driver lacks a Vulkan feature AMD FSR needs, so the game's own FSR 3 runs."]);
    await expect(b.getByTestId("upscaler-status-detail")).toHaveText("Details: VK_KHR_shader_float16_int8 is missing");
    // the game closed: the last game's, and the files' buttons work again
    await mock(page, "stopGame");
    await expect(b.getByTestId("upscaler-status")).toHaveAttribute("data-heading", "last");
    await expect(amdRow.getByRole("button", { name: "Remove AMD FSR" })).toBeEnabled();
    await expect(amdRow.getByTestId("upscaler-wait")).toHaveCount(0);
    await expect(b.getByTestId("upscaler-status")).toContainText("Last game");
    // another choice since: the plan for it
    await b.getByLabel("Upscaler").selectOption("xess");
    await expect(b.getByTestId("upscaler-status")).toHaveAttribute("data-heading", "plan");
  });

  test("OptiScaler next to the game: Kiln's upscaler steps aside, said under the choice and in the tools' lines", async ({ page }) => {
    await open(page);
    await mock(page, "setKilnJson", "graphics", { upscaler: "xess" });
    await vmock(page, "install", "intel-xess", "3.0.2");
    await mock(page, "setGraphicsStepAside", [{ kind: "optiscaler", file: "dxgi.dll", frameCap: true, upscaler: true }]);
    await again(page, "#/about", "#/settings");
    const b = block(page);
    await expect(b.getByTestId("upscaler-status-line")).toHaveText(["Kiln steps aside for OptiScaler, so it does not change the game's FSR 3."]);
    await expect(page.getByTestId("graphics-step-aside")).toHaveText(
      "OptiScaler is installed (dxgi.dll): Kiln's frame cap, DLSS settings and upscaler choice step aside; the readout still works.");
    await mock(page, "setGraphicsStepAside", [{ kind: "upscaler", file: "libxess.dll", frameCap: false, upscaler: true }]);
    await expect(page.getByTestId("graphics-step-aside")).toHaveText(
      "Another upscaler tool's files are next to the game (libxess.dll): Kiln's upscaler choice steps aside; the frame cap, DLSS settings and readout still work.");
    await expect(b.getByTestId("upscaler-status-line")).toContainText(["Kiln steps aside for another upscaler tool"]);
  });
});

test.describe("Steam Deck: Settings", () => {
  test.use({ viewport: { width: 1280, height: 800 } });

  async function pad(page: Page, button: string) {
    for (const pressed of [true, false])
      await page.evaluate(([b, p]) => (window as unknown as { __kilnMock: { emit: (n: string, v: unknown) => void } }).__kilnMock
        .emit("kiln://gamepad", { kind: "button", button: b, pressed: p, repeat: false, device: "event9" }), [button, pressed] as const);
    await page.waitForTimeout(60);
  }
  const row = (page: Page, id: string) => page.locator(`[data-deck-id="${id}"]`);

  test("choose, download with the licence screen, progress, the status row, Remove", async ({ page }) => {
    await page.goto("/?deck=steamdeck#/deck/settings");
    await expect(page.locator('[data-deck-screen="settings"]')).toBeVisible();
    await vmock(page, "pace", 120);
    const choice = row(page, "set-upscaler");
    await expect(choice).toContainText("Upscaler behind the game's FSR 3");
    await expect(choice).toContainText("The game's own FSR 3");
    await expect(row(page, "set-upscaler-now")).toHaveCount(0);
    // A: the next choice, written to kiln.json (the row waits for kiln.json: focus it once it can take the focus)
    await expect(choice).toBeEnabled();
    await choice.focus();
    await expect(choice).toBeFocused();
    await pad(page, "a");
    await expect(choice).toContainText("AMD FSR");
    await expect.poll(async () => (await kilnJson(page)).upscaler).toBe("amd");
    await expect(row(page, "set-amd-upscaler")).toContainText("Auto (FSR 4 where it runs)");
    await row(page, "set-amd-upscaler").focus();
    await pad(page, "a");
    await expect(row(page, "set-amd-upscaler")).toContainText("FSR 3.1");
    await expect.poll(async () => (await kilnJson(page)).amdUpscaler).toBe("fsr3");
    await pad(page, "a");
    await expect.poll(async () => (await kilnJson(page)).amdUpscaler).toBe("auto");
    const now = row(page, "set-upscaler-now");
    await expect(now).toContainText("When you play");
    // the rows' long lines wrap: the screen stays the Deck's 1280 pixels wide
    expect(await page.evaluate(() => Math.round(document.querySelector(".deck-top")!.getBoundingClientRect().right))).toBeLessThanOrEqual(1280);
    await expect(now).toContainText("AMD FSR's files are not downloaded yet, so the game's own FSR 3 runs.");

    // the files' row: A opens the licence screen; the right stick scrolls the text; B goes back without a download
    const amd = row(page, "set-vendor-amd-fsr");
    await expect(amd).toContainText("AMD FSR 2.4.0");
    await expect(amd).toContainText("Not downloaded");
    await amd.focus();
    await pad(page, "a");
    const lic = page.locator('[data-deck-dialog="vendor-license"]');
    await expect(lic).toBeVisible();
    await expect(lic.getByTestId("deck-license")).toContainText("Permission is hereby granted, free of charge");
    await expect(lic.getByTestId("deck-license-privacy")).toContainText("raw.githubusercontent.com sees your IP address");
    await expect(lic).not.toContainText(/dlss/i);
    const text = lic.getByTestId("deck-license");
    const top = await text.evaluate((el) => el.scrollTop);
    for (let i = 0; i < 4; i++)
      await page.evaluate((v) => (window as unknown as { __kilnMock: { emit: (n: string, v: unknown) => void } }).__kilnMock
        .emit("kiln://gamepad", { kind: "axis", axis: "ry", value: v, device: "event9" }), 1);
    await expect.poll(() => text.evaluate((el) => el.scrollTop)).toBeGreaterThan(top);
    await shot(page, "deck-license");
    await pad(page, "b");
    await expect(lic).toHaveCount(0);
    expect((await vmock<{ cmd: string }[]>(page, "calls")).filter((c) => c.cmd === "vendor_download")).toEqual([]);
    // again, and agree: progress on the row, then Downloaded, and the status row's plan
    await amd.focus();
    await pad(page, "a");
    await lic.getByRole("button", { name: "I agree, download" }).click();
    await expect(amd).toContainText("Downloading");
    await shot(page, "deck-downloading");
    await expect(amd).toContainText("Downloaded", { timeout: 15_000 });
    await expect(now).toContainText("Kiln runs AMD FSR in its place");
    await shot(page, "deck-downloaded");
    // A on a downloaded row asks to remove; Remove
    await amd.focus();
    await pad(page, "a");
    const rm = page.locator('[data-deck-dialog="vendor-remove"]');
    await expect(rm).toContainText("Kiln deletes kiln/vendor/amd/2.4.0 (70 MB) from the game folder.");
    await rm.getByRole("button", { name: "Remove" }).click();
    await expect(amd).toContainText("Not downloaded");
  });

  test("the status row says what the running game runs", async ({ page }) => {
    await page.goto("/?deck=steamdeck#/deck/settings");
    await expect(page.locator('[data-deck-screen="settings"]')).toBeVisible();
    await mock(page, "setKilnJson", "graphics", { upscaler: "amd" });
    await vmock(page, "install", "amd-fsr", "2.4.0");
    await mock(page, "setGraphicsState", state({ running: "amdFsr31", version: "3.1.6" }));
    await mock(page, "startOutside", 5);
    await again(page, "#/deck/help", "#/deck/settings");
    await expect(page.locator('[data-deck-screen="settings"]')).toBeVisible();
    const now = row(page, "set-upscaler-now");
    await expect(now).toContainText("Now");
    await expect(now).toContainText("AMD FSR 3.1 runs in place of the game's FSR 3. Version 3.1.6. AMD's selection chose FSR 3.1: FSR 4 does not run on this graphics card or driver.");
    // the files' row while the game runs: it keeps the focus, A asks nothing, and it says why
    const amd = row(page, "set-vendor-amd-fsr");
    await expect(amd.getByTestId("deck-vendor-wait")).toHaveText("Close the game to download or remove these files: the launcher does not change the game's files while it runs.");
    await amd.focus();
    await pad(page, "a");
    await expect(amd).toBeFocused();
    await expect(page.locator('[data-deck-dialog="vendor-remove"]')).toHaveCount(0);
    expect((await vmock<{ cmd: string }[]>(page, "calls")).filter((c) => c.cmd === "vendor_remove" || c.cmd === "vendor_download")).toEqual([]);
  });
});
