// "What's in this report" (docs/kiln/launcher/support-report.md): Save report... and Send to Kiln support show every file
// of the report grouped, what was replaced in each, summary.txt, and let the player leave other mods' logs out (never
// Kiln's own). Desktop and Steam Deck, in the browser mock (src/api/mockReport.ts). SHOTS=<dir> also saves screenshots.
import { expect, test, type Page } from "@playwright/test";

const OUT = process.env.SHOTS ?? "";
const shot = async (page: Page, name: string) => { if (OUT) await page.screenshot({ path: `${OUT}/${name}.png` }); };
type Mock = { reportSaves: () => string[][]; community: { requests: () => string[] } };
const reportSaves = (page: Page) => page.evaluate(() => (window as unknown as { __kilnMock: Mock }).__kilnMock.reportSaves());

async function open(page: Page, url: string) {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto(url);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

test.use({ viewport: { width: 1280, height: 800 } });

test("Save report... shows what is in the report first; other mods' logs can be left out, Kiln's cannot", async ({ page }) => {
  await open(page, "/?mock=1#/logs");
  await page.getByTestId("support-report").getByRole("button", { name: "Save report..." }).click();
  const dlg = page.getByRole("dialog", { name: "What's in this report" });
  await expect(dlg).toBeVisible();
  await expect(page.getByTestId("krc-total")).toHaveText(/^13 files, \S+ \S+ before compression$/);
  // grouped, each with its plain line and what was replaced
  for (const g of ["Kiln's logs", "Other mods' logs", "Settings", "Folder listings"]) await expect(dlg.locator(".krc-group-head", { hasText: g })).toBeVisible();
  const kilnLog = dlg.locator('[data-path="game/kiln/logs/kiln.log"]');
  await expect(kilnLog).toContainText("Kiln's main log from the last game start");
  await expect(kilnLog).toContainText("Replaced: your home folder (169 times), Steam IDs (2 times), player names (3 times)");
  await expect(kilnLog.getByRole("checkbox")).toHaveCount(0);
  await expect(dlg.locator('[data-path="listings/game-folder.txt"]')).toContainText("Nothing personal found in it.");
  await expect(page.getByTestId("krc-group-kiln")).toContainText("Support needs these to see what went wrong, so they always stay in.");
  await expect(page.getByTestId("krc-never")).toContainText("Your saves and worlds");
  await shot(page, "desktop-save-whats-in-this-report");
  // leave Building Tools' log out
  const bt = dlg.locator('[data-path="game/BuildingTools.log"]');
  await expect(bt).toContainText("Building Tools' own log");
  await bt.getByRole("checkbox", { name: "Leave out: game/BuildingTools.log" }).check();
  await expect(bt).toContainText("Left out: not in the zip.");
  await expect(page.getByTestId("krc-total")).toContainText("12 files");
  await expect(page.getByTestId("krc-total")).toContainText("1 file left out");
  await shot(page, "desktop-save-left-out");
  // summary.txt
  await page.getByTestId("krc-view").click();
  await expect(page.getByTestId("krc-summary")).toContainText("Kiln support report");
  await shot(page, "desktop-save-view-summary");
  await page.getByRole("dialog", { name: "summary.txt" }).getByRole("button", { name: "Back" }).click();
  await expect(bt.getByRole("checkbox")).toBeChecked();
  await page.getByTestId("krc-save").click();
  await expect(page.locator(".k-toasts")).toContainText("Report saved");
  expect(await reportSaves(page)).toEqual([["game/BuildingTools.log"]]);
});

test("Cancel saves nothing; Leave out all takes every other mod's log", async ({ page }) => {
  await open(page, "/?mock=1#/logs");
  const block = page.getByTestId("support-report");
  await block.getByRole("button", { name: "Save report..." }).click();
  const dlg = page.getByRole("dialog", { name: "What's in this report" });
  await dlg.getByRole("button", { name: "Cancel" }).click();
  await expect(dlg).toHaveCount(0);
  expect(await reportSaves(page)).toEqual([]);
  await block.getByRole("button", { name: "Save report..." }).click();
  await page.getByTestId("krc-leave-all").check();
  await expect(page.getByTestId("krc-total")).toContainText("2 files left out");
  await page.getByTestId("krc-save").click();
  await expect(page.locator(".k-toasts")).toContainText("Report saved");
  expect((await reportSaves(page))[0].sort()).toEqual(["game/BuildingTools.log", "game/shroudlink.log"]);
});

test("Send to Kiln support: the check step says what is in the zip; a log left out is not in what is sent", async ({ page }) => {
  await open(page, "/?mock=1&discord=in#/logs");
  await page.getByTestId("support-report").getByRole("button", { name: "Send to Kiln support" }).click();
  await page.locator("#kc-line").fill("Mods don't load since the game update");
  await page.getByTestId("kc-next").click();
    await expect(page.getByTestId("kc-replaced")).toContainText("your home folder (249 times)");
  await page.getByTestId("kc-files-toggle").click();
  const files = page.getByTestId("kc-files");
  await expect(files.locator('[data-path="game/shroudlink.log"]')).toContainText("ShroudLink's own log");
  await files.locator('[data-path="game/shroudlink.log"]').getByRole("checkbox").check();
  await expect(page.getByTestId("kc-replaced")).toContainText("1 file left out");
  await expect(page.getByTestId("kc-check")).toContainText("12 files");
  await expect(page.getByTestId("kc-send-now")).toBeEnabled();
  await page.getByTestId("krc-view").click();
  await expect(page.getByTestId("krc-summary")).toContainText("Kiln support report");
  await page.locator(".k-modal-body").evaluate((el) => el.scrollTo(0, 0));
  await shot(page, "desktop-send-check-whats-in-this-report");
  await page.getByTestId("kc-send-now").click();
  await expect(page.getByTestId("kc-sent")).toBeVisible();
});

test.describe("Steam Deck", () => {
  test("What's in this report with the pad: leave a mod's log out with A, view summary.txt, send", async ({ page }) => {
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.goto("/?mock=1&deck=steamdeck&discord=in#/deck/community");
    await page.locator('[data-deck-id="send"]').click();
    await page.locator('[data-deck-id="kind-mod"]').click();
    await page.locator('[data-deck-id="when-play"]').click();
    const check = page.getByTestId("deck-send-check");
    await expect(check.locator('[data-deck-id="files"]')).toContainText("What's in this report");
    await expect(check.locator('[data-deck-id="files"]')).toContainText("13 files");
    await check.locator('[data-deck-id="files"]').click();
    const list = page.getByTestId("deck-what-is-sent");
    await expect(list).toContainText("game/kiln/logs/kiln.log");
    await expect(page.getByTestId("deck-group-mods")).toContainText("Other mods' logs");
    const bt = page.locator('[data-deck-id="file-game/BuildingTools.log"]');
    await expect(bt).toContainText("Building Tools' own log");
    await expect(bt).toContainText("In");
    await shot(page, "deck-whats-in-this-report");
    // A on the row (the pad's A is a click)
    await bt.focus();
    await page.keyboard.press("Enter");
    await expect(bt).toContainText("Left out");
    await expect(page.getByTestId("deck-contents-total")).toContainText("1 file left out");
    await shot(page, "deck-whats-in-this-report-left-out");
    // Kiln's own log has no switch: A only reads
    await expect(page.locator('[data-deck-id="file-game/kiln/logs/kiln.log"]')).not.toContainText("Left out");
    await page.locator('[data-deck-id="view-summary"]').click();
    await expect(page.getByTestId("deck-summary")).toContainText("Kiln support report");
    await shot(page, "deck-view-summary");
    await page.locator('[data-deck-dialog="send-summary"]').getByRole("button", { name: "OK", exact: true }).click();
    await page.locator('[data-deck-dialog="send-files"]').getByRole("button", { name: "OK", exact: true }).click();
    await expect(check.locator('[data-deck-id="files"]')).toContainText("1 file left out");
    await page.locator('[data-deck-dialog="send-check"]').getByRole("button", { name: "Send to Kiln support" }).click();
    await expect(page.getByTestId("deck-sent")).toBeVisible();
  });
});
