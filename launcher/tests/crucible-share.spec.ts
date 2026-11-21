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

test("not signed in: the confirm, sign in with Discord, then the share starts by itself", async ({ page }) => {
  await open(page, "/?mock=1#/");
  await page.getByTestId("crucible-share").click();
  await expect(page.getByTestId("crucible-share-signin-first")).toHaveText("Sign in with Discord to share. Your friends don't need to.");
  await expect(page.getByTestId("crucible-share-confirm")).toHaveText("Sign in and share");
  await shot(page, "desktop-share-signin");
  expect((await requests(page)).filter((r) => r.cmd === "crucible_share")).toHaveLength(0);
  await page.getByTestId("crucible-share-confirm").click();
  await page.getByTestId("kc-signin-open").click();
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

test("the pack changed since it was shared: the pill, then Update link (same link, version 2)", async ({ page }) => {
  await open(page, "/?mock=1&discord=in&crucible=shared#/");
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

test("Crucible off in this build: no share button", async ({ page }) => {
  await page.goto("/?mock=1&crucible=off#/");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.locator(".k-hero")).toBeVisible();
  await page.waitForTimeout(500);
  await expect(page.getByTestId("crucible-share")).toHaveCount(0);
});
