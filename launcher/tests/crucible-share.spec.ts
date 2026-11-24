// Crucible on Play, sharing (src/crucible/Share.tsx; DESIGN 1.2) in mock mode: "What is Crucible?" (the wiki page), the
// first share's confirm (what goes up and about how much, what never does, link-only; Cancel sends nothing), then the
// upload and the link on the clipboard with the code; not signed in (the confirm, the Discord sign-in, then the share
// goes on by itself; Save as file instead), and a pack that changed since it was shared ("Pack changed", Update link,
// no confirm). KILN_CRUCIBLE_SHOTS=<dir> saves
// screenshots there.
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";

test.use({ permissions: ["clipboard-read", "clipboard-write"] });
const SHOTS = process.env.KILN_CRUCIBLE_SHOTS;
const shot = async (page: Page, name: string) => { if (SHOTS) { mkdirSync(SHOTS, { recursive: true }); await page.screenshot({ path: `${SHOTS}/${name}.png` }); } };
const requests = (page: Page) => page.evaluate(() => (window as unknown as { __kilnCrucibleMock: { requests: () => { cmd: string; args: Record<string, unknown> }[] } }).__kilnCrucibleMock.requests());

async function open(page: Page, url: string) {
  await page.goto(url);
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.getByTestId("crucible-share")).toBeVisible();
}

test("What is Crucible? opens the wiki page in the browser", async ({ page }) => {
  await open(page, "/?mock=1#/");
  await expect(page.getByTestId("crucible-what")).toHaveText("What is Crucible?");
  await page.getByTestId("crucible-what").click();
  await expect.poll(() => page.evaluate(() => (window as unknown as { __kilnMock: { opened: () => string[] } }).__kilnMock.opened()))
    .toContain("https://frostdev.io/kiln/wiki/crucible-share");
  expect((await requests(page)).filter((r) => r.cmd === "crucible_share")).toHaveLength(0);
});

test("signed in: the first share asks first; Cancel sends nothing", async ({ page }) => {
  await open(page, "/?mock=1&discord=in#/");
  await page.getByTestId("crucible-share").click();
  const pop = page.getByTestId("crucible-share-pop");
  await expect(pop).toContainText("Share Ember run to Crucible?");
  await expect(page.getByTestId("crucible-share-goes")).toHaveText(/^Goes up: the mod list, and the files of its \d+ mods that Crucible doesn't have yet \(up to [\d.]+ [KM]B\)\.$/);
  await expect(page.getByTestId("crucible-share-never")).toHaveText("Never sent: your saves, worlds, characters or any other personal file.");
  await expect(page.getByTestId("crucible-share-link-only")).toBeChecked();
  await expect(page.getByTestId("crucible-share-public")).toBeDisabled();
  await expect(page.getByTestId("crucible-share-confirm")).toHaveText("Share");
  await shot(page, "desktop-share-confirm");
  await page.waitForTimeout(400);
  expect((await requests(page)).filter((r) => r.cmd === "crucible_share")).toHaveLength(0);
  await page.getByTestId("crucible-share-cancel").click();
  await expect(pop).toHaveCount(0);
  expect((await requests(page)).filter((r) => r.cmd === "crucible_share")).toHaveLength(0);
});

test("signed in: Share in the confirm uploads the loaded profile and copies its link", async ({ page }) => {
  await open(page, "/?mock=1&discord=in#/");
  await page.getByTestId("crucible-share").click();
  await page.getByTestId("crucible-share-confirm").click();
  await expect(page.getByTestId("crucible-share-progress")).toContainText("Uploading");
  await shot(page, "desktop-share-uploading");
  const pop = page.getByTestId("crucible-share-pop");
  await expect(pop).toContainText("Link copied. Paste it to your friends.", { timeout: 15_000 });
  await expect(page.getByTestId("crucible-link")).toHaveText("frostdev.io/kiln/crucible/p/H7QK-3MZP");
  await expect(page.getByTestId("crucible-code")).toHaveText("H7QK-3MZP");
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe("https://frostdev.io/kiln/crucible/p/H7QK-3MZP");
  await shot(page, "desktop-share-done");
  const share = (await requests(page)).filter((r) => r.cmd === "crucible_share");
  expect(share).toHaveLength(1);
  expect(share[0].args.request).toEqual({ profile: "ember-run" });
  // the code's own Copy
  await page.getByTestId("crucible-copy-code").click();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe("H7QK-3MZP");
  // a second click shows the same link again, without another upload
  await page.getByTestId("crucible-share").click();
  await expect(pop).toHaveCount(0);
  await page.getByTestId("crucible-share").click();
  await expect(page.getByTestId("crucible-link")).toHaveText("frostdev.io/kiln/crucible/p/H7QK-3MZP");
  expect((await requests(page)).filter((r) => r.cmd === "crucible_share")).toHaveLength(1);
});

test("offline while sharing: the error speaks to the sharer (check the internet, or Save as file), Retry is there", async ({ page }) => {
  await open(page, "/?mock=1&discord=in#/");
  await page.evaluate(() => (window as unknown as { __kilnCrucibleMock: { setOnline: (on: boolean) => void } }).__kilnCrucibleMock.setOnline(false));
  await page.getByTestId("crucible-share").click();
  await page.getByTestId("crucible-share-confirm").click();
  await expect(page.getByTestId("crucible-share-error")).toHaveText("Kiln can't reach Crucible. Check your internet and try again, or use Save as file and send your friends the file.", { timeout: 15_000 });
  await expect(page.getByTestId("crucible-share-pop").getByRole("button", { name: "Retry" })).toBeVisible();
  await expect(page.getByTestId("crucible-save-file")).toBeVisible();
});

test("not signed in: confirm creates an unlisted link without opening Discord", async ({ page }) => {
  await open(page, "/?mock=1#/");
  await page.getByTestId("crucible-share").click();
  await expect(page.getByTestId("crucible-share-signin-first")).toHaveCount(0);
  await expect(page.getByTestId("crucible-share-confirm")).toHaveText("Share");
  await expect(page.getByTestId("crucible-share-confirm-text")).toContainText("Anyone with the link or code can install");
  await shot(page, "desktop-share-signin");
  expect((await requests(page)).filter((r) => r.cmd === "crucible_share")).toHaveLength(0);
  await page.getByTestId("crucible-share-confirm").click();
  await expect(page.getByTestId("kc-signin-open")).toHaveCount(0);
  await expect(page.getByTestId("crucible-share-pop")).toContainText("Link copied", { timeout: 20_000 });
  await expect(page.getByTestId("crucible-code")).toHaveText("H7QK-3MZP");
});

test("not signed in: Save as file instead builds a .kilnpack in the folder picked", async ({ page }) => {
  await open(page, "/?mock=1#/");
  await page.getByTestId("crucible-share").click();
  await page.getByTestId("crucible-save-file").click();
  await expect(page.locator(".k-toast").filter({ hasText: "Saved Ember run as a file" })).toContainText("/mnt/games/ember-run-1.0.0.kilnpack");
  const build = (await requests(page)).find((r) => r.cmd === "pack_build");
  expect(build?.args).toEqual({ request: { profile: "ember-run", version: "1.0.0", name: "Ember run" }, outDir: "/mnt/games" });
});

for (const signedIn of [false, true]) test(`the pack changed: same link, version 2 (${signedIn ? "account" : "signed-out capability"})`, async ({ page }) => {
  await open(page, `/?mock=1${signedIn ? "&discord=in" : ""}&crucible=shared#/`);
  await expect(page.getByTestId("crucible-pack-changed")).toHaveText("Pack changed");
  await page.getByTestId("crucible-share").click();
  await expect(page.getByTestId("crucible-share-changed")).toContainText("Update the link for friends? 1 added, 1 updated");
  // shared before: no confirm
  await expect(page.getByTestId("crucible-share-confirm")).toHaveCount(0);
  await shot(page, "desktop-share-changed");
  await page.getByTestId("crucible-update-link").click();
  await expect(page.getByTestId("crucible-share-pop")).toContainText("Link copied", { timeout: 15_000 });
  await expect(page.getByTestId("crucible-share-pop")).toContainText("Version 2");
  await expect(page.getByTestId("crucible-link")).toHaveText("frostdev.io/kiln/crucible/p/H7QK-3MZP");
  await expect(page.getByTestId("crucible-pack-changed")).toHaveCount(0);
});

test("an existing account-owned share signed out still offers sign-in, not an unauthorized update", async ({ page }) => {
  await open(page, "/?mock=1&crucible=shared,accountshare#/");
  await page.getByTestId("crucible-share").click();
  await expect(page.getByTestId("crucible-share-changed")).toContainText("Sign in with Discord to update this account-owned share");
  await expect(page.getByTestId("crucible-update-link")).toHaveCount(0);
  expect((await requests(page)).filter((r) => r.cmd === "crucible_share")).toHaveLength(0);
});

test("a rejected cached account session shows an actionable sign-in rather than a spinner", async ({ page }) => {
  await open(page, "/?mock=1&discord=in&crucible=shared#/");
  await page.evaluate(() => (window as unknown as { __kilnMock: { planCommand: (cmd: string, plan: unknown) => void } }).__kilnMock.planCommand("crucible_share", { error: { code: "auth_failed", message: "This sign-in has ended." } }));
  await page.getByTestId("crucible-share").click();
  await page.getByTestId("crucible-update-link").click();
  await expect(page.getByTestId("crucible-share-changed").getByRole("button", { name: "Sign in with Discord" })).toBeVisible();
  await expect(page.getByTestId("crucible-share-progress")).toHaveCount(0);
});

test("Crucible off in this build: no share button", async ({ page }) => {
  await page.goto("/?mock=1&crucible=off#/");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.locator(".k-hero")).toBeVisible();
  await page.waitForTimeout(500);
  await expect(page.getByTestId("crucible-share")).toHaveCount(0);
});


for (const taskFailure of [false, true]) test(`a nested Library edit found while sharing offers take-over and retries with the updated profile (${taskFailure ? "task" : "command"})`, async ({ page }) => {
  await open(page, "/?mock=1&discord=in#/");
  await page.evaluate(async (taskFailure) => {
    const api = await import("/src/api/commands.ts");
    const p = await api.profileGet("ember-run");
    const entry = { kind: "shroudforge", ...p.mods.shroudforge[0] };
    const mock = (window as unknown as { __kilnMock: { editInLibrary: (kind: string, id: string, change: unknown) => void; planCommand: (cmd: string, plan: unknown) => void } }).__kilnMock;
    // The displayed Library query is still cached: only the share's deep integrity check sees the edit.
    mock.editInLibrary(entry.kind, entry.id, { name: "Better Chests edited", version: "0.4.2" });
    const error = { code: "hash_mismatch" as const, message: "Better Chests was changed in its folder", detail: { libraryChanged: entry, name: "Better Chests" } };
    if (taskFailure) {
      mock.planCommand("crucible_share", { result: "share-changed-task" });
      const { useTasks } = await import("/src/ui/TaskProgress.tsx");
      useTasks.getState().upsert({ id: "share-changed-task", kind: "crucible_share", title: "Share Ember run", state: "failed", progress: null,
        message: error.message, started: new Date().toISOString(), finished: new Date().toISOString(), error });
    } else mock.planCommand("crucible_share", { error });
  }, taskFailure);
  await expect(page.locator(".k-hero")).not.toContainText("Changed on disk");
  await page.getByTestId("crucible-share").click();
  await page.getByTestId("crucible-share-confirm").click();
  const recovery = page.locator(".k-toast", { hasText: "Better Chests was changed in its folder" });
  await expect(recovery).toBeVisible();
  await expect(page.getByTestId("crucible-share-error")).toHaveText("Better Chests was changed in its folder");
  await recovery.getByRole("button", { name: "Use the changed files" }).click();
  await expect(page.getByTestId("crucible-share-pop")).toContainText("Link copied", { timeout: 15_000 });
  const now = await page.evaluate(async () => (await (await import("/src/api/commands.ts")).profileGet("ember-run")).mods.shroudforge[0]);
  expect(now.version).toBe("0.4.2");
  expect((await requests(page)).filter((r) => r.cmd === "crucible_share")).toHaveLength(1); // failed planned command was intercepted before the Crucible mock
});


test("Save as file offers the same Library recovery and retries in the chosen folder", async ({ page }) => {
  await open(page, "/?mock=1#/");
  await page.evaluate(async () => {
    const p = await (await import("/src/api/commands.ts")).profileGet("ember-run");
    const entry = { kind: "shroudforge", ...p.mods.shroudforge[0] };
    const mock = (window as unknown as { __kilnMock: { editInLibrary: (kind: string, id: string, change: unknown) => void; planCommand: (cmd: string, plan: unknown) => void } }).__kilnMock;
    mock.editInLibrary(entry.kind, entry.id, { name: "Better Chests edited", version: "0.4.2" });
    mock.planCommand("pack_build", { error: { code: "hash_mismatch", message: "Better Chests was changed in its folder", detail: { libraryChanged: entry, name: "Better Chests" } } });
  });
  await page.getByTestId("crucible-share").click();
  await page.getByTestId("crucible-save-file").click();
  const recovery = page.locator(".k-toast", { hasText: "Better Chests was changed in its folder" });
  await expect(recovery.getByRole("button", { name: "Use the changed files" })).toBeVisible();
  await recovery.getByRole("button", { name: "Use the changed files" }).click();
  await expect(page.locator(".k-toast", { hasText: "Saved Ember run as a file" })).toContainText("/mnt/games/ember-run-1.0.0.kilnpack");
  const build = (await requests(page)).find((r) => r.cmd === "pack_build");
  expect(build?.args.outDir).toBe("/mnt/games");
});
