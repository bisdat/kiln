// Crucible on Play, receiving (src/crucible/Preview.tsx; DESIGN 1.3, 1.6) in mock mode: a code (typed, or the
// clipboard's), a kiln:// link, a dropped .kilnpack; the preview's facts (Crucible's check line: passed, things to look
// at, still running with Refresh), the warning that always shows, the trust boxes, optional mods, the new profile's
// name (the pack and its creator, never a name a profile has), Install and play and its Undo, Install alone (also while
// the game runs: it never touches the game folder); still uploading with Wait, Kiln too old, taken down, offline.
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";

test.use({ permissions: ["clipboard-read", "clipboard-write"] });
const SHOTS = process.env.KILN_CRUCIBLE_SHOTS;
const shot = async (page: Page, name: string) => { if (SHOTS) { mkdirSync(SHOTS, { recursive: true }); await page.screenshot({ path: `${SHOTS}/${name}.png` }); } };
type Hooks = { requests: () => { cmd: string; args: Record<string, unknown> }[]; link: (u: string) => void; setOnline: (on: boolean) => void };
const crucible = <T,>(page: Page, f: (h: Hooks) => T) => page.evaluate((src) => (new Function("h", `return (${src})(h)`))((window as unknown as { __kilnCrucibleMock: Hooks }).__kilnCrucibleMock), f.toString()) as Promise<T>;

/** The preview's dialog (its title is the pack's name). */
const dialogOf = (page: Page) => page.getByRole("dialog").filter({ has: page.getByTestId("crucible-preview") });

async function open(page: Page, url = "/?mock=1#/") {
  await page.goto(url);
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.getByTestId("crucible-got-code")).toBeVisible();
}
async function code(page: Page, text: string) {
  await page.getByTestId("crucible-got-code").click();
  await page.getByTestId("crucible-code-input").fill(text);
  await page.getByTestId("crucible-code-open").click();
}

test("a code: the preview, trust, optional mods, Install and play, then Undo", async ({ page }) => {
  await open(page);
  await code(page, "k4tw 9xrz");
  await expect(page.getByTestId("crucible-code-error")).toHaveText("We couldn't find that code. Codes look like H7QK-3MZP.");
  await page.getByTestId("crucible-code-input").fill("k4tw9xrb");
  await expect(page.getByTestId("crucible-code-input")).toHaveValue("K4TW-9XRB");
  await page.getByTestId("crucible-code-open").click();
  const pv = page.getByTestId("crucible-preview");
  await expect(dialogOf(page)).toContainText("Cinder Trails"); // the dialog's title names the pack
  await expect(pv).toContainText("by Cinderhawk");
  await expect(page.getByTestId("crucible-fit")).toHaveText("Fits your game (build 26404966)");
  await expect(page.getByTestId("crucible-size")).toContainText("Download");
  await expect(page.getByTestId("crucible-checks")).toHaveText("Checks: passed");
  await expect(page.getByTestId("crucible-profile-name")).toHaveValue("Cinder Trails (Cinderhawk's pack)");
  await expect(pv).toContainText("Installs as its own profile. Your current mods stay as they are.");
  await expect(page.getByTestId("crucible-warning")).toHaveText("This pack contains mods, including code, from other people. Crucible checks packs automatically but does not review them.");
  await expect(page.getByTestId("crucible-trust")).not.toBeChecked();
  await expect(page.getByTestId("crucible-install")).toBeDisabled();
  // why Install waits is said under the boxes, not only in the disabled buttons' tooltips
  await expect(page.getByTestId("crucible-tick-first")).toHaveText('Tick "I trust" first.');
  await page.getByTestId("crucible-mods-toggle").click();
  await expect(page.getByTestId("crucible-mods")).toContainText("Lantern Glow");
  await expect(page.getByTestId("crucible-mods")).toContainText("by emberwright");
  await page.getByTestId("crucible-optional").getByText("Hardcore Hunger").click();
  await expect(page.getByTestId("crucible-install-only")).toBeDisabled();
  await page.getByTestId("crucible-trust").check();
  await page.getByTestId("crucible-trust-always").check();
  await expect(page.getByTestId("crucible-install-only")).toBeEnabled();
  await expect(page.getByTestId("crucible-tick-first")).toHaveCount(0);
  await shot(page, "desktop-preview");
  await page.getByTestId("crucible-install").click();
  const toast = page.locator(".k-toast").filter({ hasText: "Cinder Trails (Cinderhawk's pack) is installed" });
  await expect(toast).toBeVisible({ timeout: 15_000 });
  await expect(page.getByTestId("crucible-preview")).toHaveCount(0);
  await expect(page.locator(".k-hero-profile")).toHaveText("Profile: Cinder Trails (Cinderhawk's pack)");
  const req = (await crucible(page, (h) => h.requests())).find((r) => r.cmd === "crucible_install");
  expect(req?.args).toEqual({ gameId: "client-steam", request: { target: "pv_cinder3", consent: true, trustAlways: true, optional: { "cozy-camp": true, "hardcore-hunger": true },
    name: "Cinder Trails (Cinderhawk's pack)", play: true } });
  // "and play": the Play control starts the new instance
  await expect.poll(() => page.evaluate(() => (window as unknown as { __kilnMock: { calls: () => string[] } }).__kilnMock.calls().includes("launch_play")), { timeout: 15_000 }).toBe(true);
  await page.evaluate(() => (window as unknown as { __kilnMock: { stopGame: () => void } }).__kilnMock.stopGame());
  await toast.getByRole("button", { name: "Undo" }).click();
  await expect(page.locator(".k-toast").filter({ hasText: "Back to Ember run" })).toBeVisible();
  await expect(page.locator(".k-hero-profile")).toHaveText("Profile: Ember run");
  // "Always trust" was remembered; the pack stays installed as its own profile (the preview offers it)
  expect(await page.evaluate(() => (window as unknown as { __kilnCrucibleMock: { state: () => { trust: { name: string }[] } } }).__kilnCrucibleMock.state().trust.map((x) => x.name))).toContain("Cinderhawk");
  await code(page, "K4TW-9XRB");
  await expect(page.getByTestId("crucible-has-instance")).toHaveText("You have this pack already.");
});

test("the clipboard's code is filled in", async ({ page }) => {
  await open(page);
  await page.evaluate(() => navigator.clipboard.writeText("https://frostdev.io/kiln/crucible/p/K4TW-9XRB"));
  await page.getByTestId("crucible-got-code").click();
  await expect(page.getByTestId("crucible-code-input")).toHaveValue("K4TW-9XRB");
});

test("Paste fills in a code copied after the box opened; other text is not pasted", async ({ page }) => {
  await open(page);
  await page.evaluate(() => navigator.clipboard.writeText("a shopping list"));
  await page.getByTestId("crucible-got-code").click();
  await expect(page.getByTestId("crucible-code-input")).toHaveValue("");
  await page.getByTestId("crucible-code-paste").click();
  await expect(page.locator(".k-toast").filter({ hasText: "There's no pack code on the clipboard." })).toBeVisible();
  await expect(page.getByTestId("crucible-code-input")).toHaveValue("");
  await page.evaluate(() => navigator.clipboard.writeText("h7qk 3mzp"));
  await page.getByTestId("crucible-code-paste").click();
  await expect(page.getByTestId("crucible-code-input")).toHaveValue("H7QK-3MZP");
});

test("a kiln:// link opens the preview; a trusted creator's pack already installed offers its update", async ({ page }) => {
  await open(page, "/?mock=1&crucible=packs#/");
  await crucible(page, (h) => h.link("kiln://crucible/R7MX-2HNQ"));
  await expect(dialogOf(page)).toContainText("Ashen Frontier");
  await expect(page.getByTestId("crucible-has-instance")).toHaveText("You have this pack. Version 1.1.0 is ready.");
  await page.getByTestId("crucible-preview-see-update").click();
  await expect(page.getByTestId("crucible-update")).toBeVisible();
});

test("still uploading: Wait, then Install once it is ready; another game build is a yellow note", async ({ page }) => {
  await open(page);
  await code(page, "M2PD-7FJC");
  await expect(page.getByTestId("crucible-uploading")).toContainText("Wren's pack is still uploading (62%). Wait here, and Install opens up when it's ready.");
  await shot(page, "desktop-preview-uploading");
  await page.getByRole("button", { name: "Wait" }).click();
  await expect(page.getByTestId("crucible-uploading")).toHaveCount(0, { timeout: 15_000 });
  await expect(page.getByTestId("crucible-fit")).toHaveText("Made for game build 26430253. You have 26404966. Some mods may need updates.");
  // the upload just finished: Crucible's full check still runs; Refresh asks again
  const checks = page.getByTestId("crucible-checks");
  await expect(checks).toContainText("Checks: still running on Crucible");
  await crucible(page, (h) => (h as unknown as { setChecks: (v: string, c: unknown) => void }).setChecks("pv_frost1", { state: "done", ok: true, errors: [], warnings: [] }));
  await page.getByTestId("crucible-checks-refresh").click();
  await expect(checks).toHaveText("Checks: passed");
  await page.getByTestId("crucible-trust").check();
  await expect(page.getByTestId("crucible-install")).toBeEnabled();
});

test("Kiln too old blocks; a pack taken down and offline say so", async ({ page }) => {
  await open(page);
  await code(page, "Q8NV-4KMA");
  await expect(page.getByTestId("crucible-kiln-old")).toContainText("Needs Kiln 0.2.0-dev.31 or newer.");
  await expect(page.getByTestId("crucible-checks")).toContainText("Checks: 1 thing to look at");
  await expect(page.getByTestId("crucible-checks")).toContainText("shroud-everywhere: references networking (System.Net.Http)");
  await page.getByTestId("crucible-trust").check();
  await expect(page.getByTestId("crucible-install")).toBeDisabled();
  await expect(page.getByTestId("crucible-install-only")).toBeDisabled();
  await page.keyboard.press("Escape");
  await code(page, "W3RX-8TDN");
  await expect(page.getByTestId("crucible-code-error")).toHaveText("This pack is no longer on Crucible.");
  await crucible(page, (h) => h.setOnline(false));
  await page.getByTestId("crucible-code-input").fill("K4TW-9XRB");
  await page.getByTestId("crucible-code-open").click();
  await expect(page.getByTestId("crucible-code-error")).toHaveText("Kiln can't reach Crucible. Ask your friend for the pack file (Share to Crucible > Save as file).");
});

test("a dropped .kilnpack opens the same preview and installs from the file", async ({ page }) => {
  await open(page);
  await page.evaluate(() => {
    const dt = new DataTransfer();
    dt.items.add(new File(["x"], "cinder-trails-1.2.0.kilnpack"));
    document.body.dispatchEvent(new DragEvent("drop", { dataTransfer: dt, bubbles: true, cancelable: true }));
  });
  await expect(dialogOf(page)).toContainText("Cinder Trails");
  await expect(page.getByTestId("crucible-warning")).toBeVisible();
  await expect(page.getByTestId("crucible-checks")).toHaveText("Checks: passed");
  await page.getByTestId("crucible-trust").check();
  await page.getByTestId("crucible-install").click();
  await expect(page.locator(".k-toast").filter({ hasText: "Cinder Trails (Cinderhawk's pack) is installed" })).toBeVisible({ timeout: 15_000 });
  const req = (await crucible(page, (h) => h.requests())).find((r) => r.cmd === "pack_install");
  expect((req?.args.request as { source: unknown; consent: boolean }).source).toEqual({ path: "cinder-trails-1.2.0.kilnpack" });
  expect((req?.args.request as { name?: string; activate?: boolean })).toMatchObject({ name: "Cinder Trails (Cinderhawk's pack)", activate: true });
});

const drop = (page: Page, name: string) => page.evaluate((n) => {
  const dt = new DataTransfer();
  dt.items.add(new File(["x"], n));
  document.body.dispatchEvent(new DragEvent("drop", { dataTransfer: dt, bubbles: true, cancelable: true }));
}, name);

test("a dropped file is checked here: an error blocks the install, a warning is shown", async ({ page }) => {
  await open(page);
  await drop(page, "cinder-trails-unsafe.kilnpack");
  const checks = page.getByTestId("crucible-checks");
  await expect(checks).toContainText("1 problem stops the install");
  await expect(checks).toContainText('entry "../evil.dll" has an unsafe path');
  await page.getByTestId("crucible-trust").check();
  await expect(page.getByTestId("crucible-install")).toBeDisabled();
  await expect(page.getByTestId("crucible-install-only")).toBeDisabled();
  await expect(page.getByTestId("crucible-block")).toHaveText("The pack failed its checks: it can't be installed.");
  await page.getByRole("button", { name: "Cancel" }).click();
  await drop(page, "cinder-trails-warn.kilnpack");
  await expect(page.getByTestId("crucible-checks")).toContainText("1 thing to look at");
  await expect(page.getByTestId("crucible-checks")).toContainText("fps-unlock: references networking");
  await page.getByTestId("crucible-trust").check();
  await expect(page.getByTestId("crucible-install")).toBeEnabled();
});

test("Install alone while the game runs: the profile is made, nothing is loaded or started", async ({ page }) => {
  await open(page);
  await page.evaluate(() => (window as unknown as { __kilnMock: { externalStart: (id: string) => void } }).__kilnMock.externalStart("client-steam"));
  await code(page, "K4TW-9XRB");
  await expect(dialogOf(page)).toContainText("Cinder Trails");
  await page.getByTestId("crucible-trust").check();
  await expect(page.getByTestId("crucible-install")).toBeDisabled();
  await expect(page.getByTestId("crucible-install-only")).toBeEnabled();
  await expect(page.getByTestId("crucible-block")).toHaveText("Enshrouded is running. Install adds the pack now; load it once you close the game.");
  await shot(page, "desktop-preview-install-only");
  await page.getByTestId("crucible-install-only").click();
  const toast = page.locator(".k-toast").filter({ hasText: "Cinder Trails (Cinderhawk's pack) is installed" });
  await expect(toast).toContainText("It's in your profiles. Load it when you want to play.", { timeout: 15_000 });
  await expect(toast.getByRole("button", { name: "Undo" })).toHaveCount(0);
  await expect(page.getByTestId("crucible-preview")).toHaveCount(0);
  await expect(page.locator(".k-hero-profile")).toHaveText("Profile: Ember run");
  const req = (await crucible(page, (h) => h.requests())).find((r) => r.cmd === "crucible_install");
  expect((req?.args.request as { play?: boolean }).play).toBe(false);
  expect(await page.evaluate(() => (window as unknown as { __kilnMock: { calls: () => string[] } }).__kilnMock.calls().filter((c) => c === "launch_play").length)).toBe(0);
  // the pack is installed as its own profile: the preview offers it
  await code(page, "K4TW-9XRB");
  await expect(page.getByTestId("crucible-has-instance")).toHaveText("You have this pack already.");
});

test("the new profile's name: a friend's pack named like a profile of mine gets the creator, and never a taken name", async ({ page }) => {
  await open(page, "/?mock=1&discord=in&crucible=shared#/");
  // the pack shared from the profile "Ember run" (by emberfox), opened where a profile "Ember run" exists too
  await code(page, "H7QK-3MZP");
  await expect(dialogOf(page)).toContainText("Ember run");
  const name = page.getByTestId("crucible-profile-name");
  await expect(name).toHaveValue("Ember run (emberfox's pack)");
  await expect(page.getByTestId("crucible-name-taken")).toHaveCount(0);
  // the player types the name of their own profile: it would be "Ember run (2)"
  await name.fill("ember run");
  await expect(page.getByTestId("crucible-name-taken")).toHaveText("You have a profile with this name. This one will be ember run (2).");
  await name.fill("Ember run, Felup's");
  await expect(page.getByTestId("crucible-name-taken")).toHaveCount(0);
  await page.getByTestId("crucible-trust").check();
  await page.getByTestId("crucible-install-only").click();
  await expect(page.locator(".k-toast").filter({ hasText: "Ember run, Felup's is installed" })).toBeVisible({ timeout: 15_000 });
  const req = (await crucible(page, (h) => h.requests())).find((r) => r.cmd === "crucible_install");
  expect((req?.args.request as { name?: string }).name).toBe("Ember run, Felup's");
  // a shared pack's check is still queued on Crucible right after the upload
  await code(page, "H7QK-3MZP");
  await expect(page.getByTestId("crucible-checks")).toContainText("Checks: still running on Crucible");
});
