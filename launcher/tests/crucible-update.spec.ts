// Crucible pack updates (src/crucible/Update.tsx, Profiles.tsx; DESIGN 1.5) in mock mode: the Play banner of the
// loaded instance, the update screen (added, updated, removed, what is kept, one conflict that needs a choice), a
// "conflict" refusal that brings the plan back, Update, Undo (pack_undo), and Uninstall in Settings > Profiles.
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";

const SHOTS = process.env.KILN_CRUCIBLE_SHOTS;
const shot = async (page: Page, name: string) => { if (SHOTS) { mkdirSync(SHOTS, { recursive: true }); await page.screenshot({ path: `${SHOTS}/${name}.png` }); } };
const requests = (page: Page) => page.evaluate(() => (window as unknown as { __kilnCrucibleMock: { requests: () => { cmd: string; args: Record<string, unknown> }[] } }).__kilnCrucibleMock.requests());

async function open(page: Page, url: string) {
  await page.goto(url);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

test("the banner on Play, the update screen with a conflict, Update and play", async ({ page }) => {
  await open(page, "/?mock=1&crucible=instance#/");
  const banner = page.getByTestId("crucible-update-banner");
  await expect(banner).toContainText("Ashen Frontier 1.0.0 → 1.1.0 by Stonewright: 1 added, 2 updated, 1 removed.");
  await shot(page, "desktop-update-banner");
  await page.getByTestId("crucible-see-changes").click();
  const screen = page.getByTestId("crucible-update");
  await expect(page.getByTestId("crucible-diff")).toContainText("Busy Hands 0.2.8");
  await expect(page.getByTestId("crucible-diff")).toContainText("0.1.7 → 0.1.8");
  await expect(page.getByTestId("crucible-diff")).toContainText("(kept in your Library)");
  await expect(page.getByTestId("crucible-kept")).toHaveText("Your changes kept: 2 settings, 2 data edits, 1 mod.");
  await expect(page.getByTestId("crucible-renamed")).toHaveText("kiln-auto-loot-keys.json is kiln-auto-loot/hotkeys.json now: your settings move with it.");
  await expect(page.getByTestId("crucible-overlaps")).toContainText("Your change still wins (1)");
  await expect(page.getByTestId("crucible-overlaps")).toContainText("The pack now sets 800. Yours, 999, still counts.");
  await expect(page.getByTestId("crucible-saves-first")).toHaveText("Your saves are backed up first.");
  await expect(page.getByTestId("crucible-conflicts")).toContainText("The pack now says 12. You had 20.");
  await expect(page.getByTestId("crucible-update-go")).toHaveText("Update and play");
  await expect(page.getByTestId("crucible-update-go")).toBeDisabled();
  await expect(page.getByTestId("crucible-choose-first")).toHaveText("Choose for each setting first.");
  await page.getByTestId("crucible-keep-mine").check();
  await shot(page, "desktop-update-screen");
  // the backend refuses with "conflict" (the pack changed meanwhile): the plan comes back, nothing is lost
  await page.evaluate(() => (window as unknown as { __kilnMock: { planCommand: (c: string, p: unknown) => void } }).__kilnMock.planCommand("crucible_update",
    { error: { code: "conflict", message: "1 setting(s) need your choice.", detail: { profile: "ashen-frontier", pack: "ashen-frontier", from: "1.0.0", to: "1.1.0",
      mods: { added: [], updated: [], removed: [], yours: [], keptSwitches: [] }, layer: { changed: true },
      files: [{ path: "kiln-auto-loot.json", side: "both", mode: "merge-patch", taken: [], kept: [], conflicts: [{ key: "smash", base: true, pack: false, mine: true }] }], conflicts: 1 } } }));
  await page.getByTestId("crucible-update-go").click();
  await expect(page.locator(".k-toast").filter({ hasText: "A setting needs your choice." })).toBeVisible();
  await expect(screen).toContainText("“smash”");
  await expect(page.getByTestId("crucible-update-go")).toBeDisabled();
  await page.getByTestId("crucible-take-pack").check();
  await page.getByTestId("crucible-update-go").click();
  await expect(page.locator(".k-toast").filter({ hasText: "Ashen Frontier is up to date (1.1.0)" })).toBeVisible({ timeout: 15_000 });
  await expect(banner).toHaveCount(0);
  const upd = (await requests(page)).filter((r) => r.cmd === "crucible_update").at(-1);
  expect(upd?.args).toEqual({ gameId: "client-steam", request: { profile: "ashen-frontier", versionId: "pv_ashen2", consent: true, resolve: { "kiln-auto-loot.json#range": "mine", "kiln-auto-loot.json#smash": "pack" } } });
});

test("dismissing the banner keeps it away for that version", async ({ page }) => {
  await open(page, "/?mock=1&crucible=instance#/");
  await page.getByTestId("crucible-banner-dismiss").click();
  await expect(page.getByTestId("crucible-update-banner")).toHaveCount(0);
  await page.reload();
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.getByTestId("crucible-share")).toBeVisible();
  await expect(page.getByTestId("crucible-update-banner")).toHaveCount(0);
});

test("Settings > Profiles: the instance's pack line, Update, Undo, then Uninstall", async ({ page }) => {
  await open(page, "/?mock=1&crucible=packs#/settings");
  const row = page.locator(".k-profiles li").filter({ hasText: "Ashen Frontier" });
  await expect(row.getByTestId("crucible-tag")).toHaveText("Crucible pack");
  await expect(row.getByTestId("crucible-pack-line")).toContainText("Ashen Frontier 1.0.0");
  await expect(page.getByTestId("crucible-installed")).toContainText("Ashen Frontier");
  await expect(page.getByTestId("crucible-no-shared")).toContainText("Nothing shared yet.");
  await shot(page, "desktop-settings-profiles");
  await row.getByTestId("crucible-profile-update").click();
  await expect(page.getByTestId("crucible-update-go")).toHaveText("Update");
  await page.getByTestId("crucible-take-pack").check();
  await page.getByTestId("crucible-update-go").click();
  const toast = page.locator(".k-toast").filter({ hasText: "Ashen Frontier is up to date (1.1.0)" });
  await expect(toast).toBeVisible({ timeout: 15_000 });
  await expect(row.getByTestId("crucible-pack-line")).toContainText("Ashen Frontier 1.1.0");
  await toast.getByRole("button", { name: "Undo" }).click();
  await expect(page.locator(".k-toast").filter({ hasText: "Ashen Frontier is back to 1.0.0" })).toBeVisible();
  await expect(row.getByTestId("crucible-pack-line")).toContainText("Ashen Frontier 1.0.0");
  await row.getByRole("button", { name: /More/ }).click();
  await page.getByRole("menuitem", { name: "Uninstall" }).click();
  await page.getByTestId("crucible-uninstall-go").click();
  await expect(page.locator(".k-toast").filter({ hasText: "Ashen Frontier is uninstalled" })).toBeVisible();
  await expect(page.locator(".k-profiles li").filter({ hasText: "Ashen Frontier" })).toHaveCount(0);
  await expect(page.getByTestId("crucible-no-installed")).toBeVisible();
});

test("Settings: the shared packs list with its link and code", async ({ page }) => {
  await open(page, "/?mock=1&crucible=shared&discord=in#/settings");
  const row = page.getByTestId("crucible-shared-row");
  await expect(row).toContainText("Ember run");
  await expect(row.getByTestId("crucible-code")).toHaveText("H7QK-3MZP");
  await expect(row).toContainText("Pack changed");
  await expect(page.getByText("Count my installs (anonymous)")).toBeVisible();
});

test("the player's data changes: still winning, Drop my change sends drop, a player layer survives the update", async ({ page }) => {
  await open(page, "/?mock=1&crucible=packs#/settings");
  const row = page.locator(".k-profiles li").filter({ hasText: "Ashen Frontier" });
  await row.getByTestId("crucible-profile-update").click();
  const overlaps = page.getByTestId("crucible-overlaps");
  await expect(overlaps).toContainText("Material_T1_Wood_Logs · maxStackSize");
  await expect(page.getByTestId("crucible-saves-first")).toHaveCount(0);
  await page.getByTestId("crucible-drop-mine").check();
  await expect(overlaps.getByText("Drop my change (use 800)")).toBeVisible();
  await page.getByTestId("crucible-keep-mine").check();
  await shot(page, "desktop-update-overlaps");
  await page.getByTestId("crucible-update-go").click();
  await expect(page.locator(".k-toast").filter({ hasText: "Ashen Frontier is up to date (1.1.0)" })).toBeVisible({ timeout: 15_000 });
  const upd = (await requests(page)).filter((r) => r.cmd === "crucible_update").at(-1);
  expect(upd?.args).toEqual({ gameId: "client-steam", request: { profile: "ashen-frontier", versionId: "pv_ashen2", consent: true,
    resolve: { "kiln-auto-loot.json#range": "mine" }, drop: ['["items","Material_T1_Wood_Logs","maxStackSize"]'] } });
});

test("unticking Drop my change sends no drop", async ({ page }) => {
  await open(page, "/?mock=1&crucible=packs#/settings");
  await page.locator(".k-profiles li").filter({ hasText: "Ashen Frontier" }).getByTestId("crucible-profile-update").click();
  await page.getByTestId("crucible-drop-mine").check();
  await page.getByTestId("crucible-drop-mine").uncheck();
  await page.getByTestId("crucible-take-pack").check();
  await page.getByTestId("crucible-update-go").click();
  await expect(page.locator(".k-toast").filter({ hasText: "Ashen Frontier is up to date (1.1.0)" })).toBeVisible({ timeout: 15_000 });
  const upd = (await requests(page)).filter((r) => r.cmd === "crucible_update").at(-1);
  expect((upd?.args.request as Record<string, unknown>).drop).toBeUndefined();
});
