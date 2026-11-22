// Settings' "WebKit's sandbox" (src-tauri/src/sandbox.rs), in mock mode: the switch writes or removes the marker for the
// next start (app_sandbox_set), says what off means and what WebKit does in this start, and turns the sandbox back on.
import { expect, test, type Locator, type Page } from "@playwright/test";

type Mock = { calls: () => string[] };
const calls = (page: Page, cmd: string) => page.evaluate((c) => (window as unknown as { __kilnMock: Mock }).__kilnMock.calls().filter((x) => x === c).length, cmd);

/** The Settings row of a switch, with its More text open (Settings' rows: one line of help, More for the rest). */
async function rowOf(page: Page, sw: Locator) {
  const r = page.locator(".k-st-row", { has: sw });
  await expect(r).toBeVisible();
  const more = r.getByRole("button", { name: "More" });
  if (await more.count()) await more.click();
  return r;
}

test("the switch turns WebKit's sandbox off for the next start and back on", async ({ page }) => {
  await page.goto("/#/settings/advanced");
  const sw = page.getByRole("switch", { name: "WebKit's sandbox" });
  await expect(sw).toBeChecked();
  const card = await rowOf(page, sw);
  await expect(card).toContainText("Applies at the next start. Off: the launcher's page runs without WebKit's process sandbox; it shows only the launcher's own pages and, from the internet, only images from Nexus Mods.");
  await expect(card).toContainText("This start: WebKit does not use its sandbox (its version for this launcher uses it only when WEBKIT_FORCE_SANDBOX=1 is set).");
  await expect(card).not.toContainText("On this system:");
  await card.locator(".k-toggle").click();
  await expect(sw).not.toBeChecked();
  await expect(page.locator(".k-toast", { hasText: "The next start turns WebKit's sandbox off" })).toBeVisible();
  await expect(card).toContainText("Turned off in Settings");
  // and back on
  await card.locator(".k-toggle").click();
  await expect(sw).toBeChecked();
  await expect(page.locator(".k-toast", { hasText: "The next start leaves WebKit's sandbox on" })).toBeVisible();
  await expect(card).not.toContainText("Turned off in Settings");
  expect(await calls(page, "app_sandbox_set")).toBe(2);
  // the render switch next to it is untouched
  await expect(page.getByRole("switch", { name: "Draw the launcher without the GPU" })).not.toBeChecked();
  expect(await calls(page, "app_render_set")).toBe(0);
});

test("after the player turned it off in a message, Settings says so and can turn it back on", async ({ page }) => {
  await page.goto("/?sandbox=off#/settings/advanced");
  const sw = page.getByRole("switch", { name: "WebKit's sandbox" });
  await expect(sw).not.toBeChecked();
  const card = await rowOf(page, sw);
  await expect(card).toContainText("This start: WebKit's sandbox is off.");
  await expect(card).toContainText("Turned off by the player in the launcher's message at the start: WebKit's sandbox could not start");
  await expect(card).toContainText("On this system: bwrap fails: bwrap: setting up uid map: Permission denied.");
  await card.locator(".k-toggle").click();
  await expect(sw).toBeChecked();
  await expect(card).not.toContainText("Turned off by the player");
  expect(await calls(page, "app_sandbox_set")).toBe(1);
});
