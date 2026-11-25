// Download missing mods (src/crucible/Preview.tsx, Profiles.tsx; crucible_repair) in mock mode: a pack profile whose
// mod the player removed from the Library. The preview opened by the same code says what is missing before Load it and
// offers Download missing mods next to See changes (an update waits: never taken silently) or next to Load it; the task
// shows its progress, the toast what came back; nothing is loaded or started, also while the game runs. Settings >
// Profiles' "…" menu has the same entry. An optional mod switched off is not missing.
import { expect, test, type Page } from "@playwright/test";

type Hooks = { requests: () => { cmd: string; args: Record<string, unknown> }[]; link: (u: string) => void; dropFromLibrary: (kind: string, id: string) => void;
  state: () => { instances: [string, { version: string }][] } };
const crucible = <T,>(page: Page, f: (h: Hooks) => T) => page.evaluate((src) => (new Function("h", `return (${src})(h)`))((window as unknown as { __kilnCrucibleMock: Hooks }).__kilnCrucibleMock), f.toString()) as Promise<T>;
type Mock = { calls: () => string[]; holdTasks: (kind: string, hold: { gate: string } | null) => void; openGate: (name: string) => void; externalStart: (id: string) => void };
const mock = <T,>(page: Page, f: (m: Mock) => T) => page.evaluate((src) => (new Function("m", `return (${src})(m)`))((window as unknown as { __kilnMock: Mock }).__kilnMock), f.toString()) as Promise<T>;
const dialogOf = (page: Page) => page.getByRole("dialog").filter({ has: page.getByTestId("crucible-preview") });

async function open(page: Page, url: string) {
  await page.goto(url);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}
const drop = (page: Page, name: string) => page.evaluate((n) => {
  const dt = new DataTransfer();
  dt.items.add(new File(["x"], n));
  document.body.dispatchEvent(new DragEvent("drop", { dataTransfer: dt, bubbles: true, cancelable: true }));
}, name);
async function code(page: Page, text: string) {
  await page.getByTestId("crucible-got-code").click();
  await page.getByTestId("crucible-code-input").fill(text);
  await page.getByTestId("crucible-code-open").click();
}

test("the same code again: the preview names the missing mod, Download missing mods puts it back, the update still waits", async ({ page }) => {
  await open(page, "/?mock=1&crucible=packs#/");
  const hero = await page.locator(".k-hero-profile").textContent();
  await crucible(page, (h) => h.dropFromLibrary("kiln", "big-stacks"));
  await crucible(page, (h) => h.link("kiln://crucible/R7MX-2HNQ"));
  await expect(dialogOf(page)).toContainText("Ashen Frontier");
  await expect(page.getByTestId("crucible-has-instance")).toHaveText("You have this pack. Version 1.1.0 is ready.");
  const note = page.getByTestId("crucible-missing");
  await expect(note).toContainText("A mod of this pack is missing");
  await expect(note).toContainText("Bigger Stacks (pack version 1.0.0) is no longer in your Library. Load it would start the game without it.");
  await expect(page.getByTestId("crucible-preview-see-update")).toBeVisible();
  const repair = page.getByTestId("crucible-repair");
  await expect(repair).toHaveText("Download the missing mod (1.1 MB)");
  await mock(page, (m) => m.holdTasks("crucible_repair", { gate: "repair" }));
  await repair.click();
  await expect(page.getByTestId("crucible-repair-progress")).toContainText(/Downloading 1 mods|Checking the files|Adding to your Library/);
  await mock(page, (m) => m.openGate("repair"));
  const toast = page.locator(".k-toast").filter({ hasText: "Ashen Frontier: the missing mod is back in your Library" });
  await expect(toast).toBeVisible({ timeout: 15_000 });
  await expect(toast).toContainText("Downloaded 1.1 MB. Nothing else changed: load the profile when you want to play.");
  await expect(note).toHaveCount(0);
  await expect(repair).toHaveCount(0);
  // the update is still the update screen's: the instance keeps 1.0.0, nothing was loaded or started
  await expect(page.getByTestId("crucible-has-instance")).toHaveText("You have this pack. Version 1.1.0 is ready.");
  await expect(page.getByTestId("crucible-preview-see-update")).toBeVisible();
  expect((await crucible(page, (h) => h.state())).instances.find(([pid]) => pid === "ashen-frontier")?.[1].version).toBe("1.0.0");
  const reqs = await crucible(page, (h) => h.requests());
  expect(reqs.filter((r) => r.cmd === "crucible_repair").map((r) => r.args)).toEqual([{ gameId: "client-steam", request: { profile: "ashen-frontier" } }]);
  expect(reqs.some((r) => r.cmd === "crucible_update" || r.cmd === "crucible_install")).toBe(false);
  const calls = await mock(page, (m) => m.calls());
  expect(calls.filter((c) => c === "profile_activate" || c === "launch_play")).toEqual([]);
  await expect(page.locator(".k-hero-profile")).toHaveText(hero ?? "");
});

test("no update, the game running: Load it waits, Download missing mods runs; a switched-off optional mod is not missing", async ({ page }) => {
  await open(page, "/?mock=1#/");
  await code(page, "K4TW-9XRB");
  await expect(dialogOf(page)).toContainText("Cinder Trails");
  await page.getByTestId("crucible-trust").check();
  await page.getByTestId("crucible-install-only").click();
  await expect(page.locator(".k-toast").filter({ hasText: "Cinder Trails (Cinderhawk's pack) is installed" })).toBeVisible({ timeout: 15_000 });
  // nothing missing: Load it as before, no note, no Download missing mods
  await code(page, "K4TW-9XRB");
  await expect(page.getByTestId("crucible-has-instance")).toHaveText("You have this pack already.");
  await expect(page.getByTestId("crucible-load-it")).toBeEnabled();
  await expect(page.getByTestId("crucible-missing")).toHaveCount(0);
  await expect(page.getByTestId("crucible-repair")).toHaveCount(0);
  await page.keyboard.press("Escape");
  // Hardcore Hunger is optional and off by default: gone from the Library, it is not missing
  await crucible(page, (h) => { h.dropFromLibrary("kiln", "weather-plus"); h.dropFromLibrary("kiln", "hardcore-hunger"); });
  await mock(page, (m) => m.externalStart("client-steam"));
  // the same code again moments later: Crucible is not asked again (the preview is fresh), the Library is (pack_lacking)
  const opens = async () => (await crucible(page, (h) => h.requests())).filter((r) => r.cmd === "crucible_open").length;
  const lacks = async () => (await crucible(page, (h) => h.requests())).filter((r) => r.cmd === "pack_lacking").length;
  const [opened, lacked] = [await opens(), await lacks()];
  await code(page, "K4TW-9XRB");
  await expect(page.getByTestId("crucible-has-instance")).toHaveText("You have this pack already.");
  await expect(page.getByTestId("crucible-missing")).toContainText("Weather Plus (pack version 1.2.0) is no longer in your Library. Load it would start the game without it.");
  expect(await opens()).toBe(opened);
  expect(await lacks()).toBeGreaterThan(lacked);
  await expect(page.getByTestId("crucible-missing")).not.toContainText("Hardcore Hunger");
  await expect(page.getByTestId("crucible-load-it")).toBeDisabled();
  await expect(page.getByTestId("crucible-repair")).toHaveText("Download the missing mod (9.6 MB)");
  await page.getByTestId("crucible-repair").click();
  await expect(page.locator(".k-toast").filter({ hasText: "Cinder Trails: the missing mod is back in your Library" })).toBeVisible({ timeout: 15_000 });
  await expect(page.getByTestId("crucible-missing")).toHaveCount(0);
  await expect(page.getByTestId("crucible-load-it")).toBeVisible();
  const calls = await mock(page, (m) => m.calls());
  expect(calls.filter((c) => c === "profile_activate" || c === "launch_play")).toEqual([]);
});

test("a pack installed from a file takes its missing mods from the same file", async ({ page }) => {
  await open(page, "/?mock=1#/");
  await drop(page, "lantern-pack-1.0.0.kilnpack");
  await expect(dialogOf(page)).toContainText("Lantern Pack");
  await page.getByTestId("crucible-trust").check();
  await page.getByTestId("crucible-install-only").click();
  await expect(page.locator(".k-toast").filter({ hasText: "Lantern Pack is installed" })).toBeVisible({ timeout: 15_000 });
  await crucible(page, (h) => h.dropFromLibrary("kiln", "lantern-glow"));
  await drop(page, "lantern-pack-1.0.0.kilnpack");
  await expect(page.getByTestId("crucible-missing")).toContainText("Lantern Glow (pack version 1.0.0) is no longer in your Library.");
  await expect(page.getByTestId("crucible-repair")).toHaveText("Add the missing mod from this file");
  await page.getByTestId("crucible-repair").click();
  const toast = page.locator(".k-toast").filter({ hasText: "Lantern Pack: the missing mod is back in your Library" });
  await expect(toast).toContainText("Nothing else changed: load the profile when you want to play.", { timeout: 15_000 });
  const req = (await crucible(page, (h) => h.requests())).filter((r) => r.cmd === "crucible_repair").at(-1);
  expect(req?.args).toEqual({ gameId: "client-steam", request: { profile: "lantern-pack", path: "lantern-pack-1.0.0.kilnpack" } });
  await expect(page.getByTestId("crucible-missing")).toHaveCount(0);
});

test("Settings > Profiles: the pack profile's menu has Download missing mods while a mod is missing", async ({ page }) => {
  await open(page, "/?mock=1&crucible=packs#/");
  await crucible(page, (h) => h.dropFromLibrary("kiln", "more-hotbars"));
  await page.evaluate(() => { location.hash = "#/settings/mods"; });
  const row = page.locator(".k-profiles li").filter({ hasText: "Ashen Frontier" });
  await expect(row.getByTestId("crucible-tag")).toHaveText("Crucible pack");
  await row.getByRole("button", { name: /More/ }).click();
  await page.getByRole("menuitem", { name: "Download the missing mod (614 KB)" }).click();
  await expect(page.locator(".k-toast").filter({ hasText: "Ashen Frontier: the missing mod is back in your Library" })).toBeVisible({ timeout: 15_000 });
  await row.getByRole("button", { name: /More/ }).click();
  await expect(page.getByRole("menuitem", { name: "Uninstall" })).toBeVisible();
  await expect(page.getByRole("menuitem", { name: /missing mod/ })).toHaveCount(0);
  const calls = await mock(page, (m) => m.calls());
  expect(calls.filter((c) => c === "profile_activate" || c === "launch_play")).toEqual([]);
});
