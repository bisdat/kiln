// Settings, ShroudForge in mock mode: what is set (default, the older switch, a choice, an
// invalid value), the next and the last start, Kiln's interpreter greyed while held with its reason and never
// ShroudForge 1.8, the dialog (Cancel writes nothing, Save one key), a late answer for another game that never saves, a
// conflicting edit, the running game, and no backend picker outside Settings.
// Screenshots go to .out/launcher/shots/ (SHOTS=0 skips them).
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";
import { join } from "node:path";

const SHOTS = join(import.meta.dirname, "..", "..", ".out", "launcher", "shots");
async function shot(page: Page, name: string) {
  if (process.env.SHOTS === "0") return;
  mkdirSync(SHOTS, { recursive: true });
  await card(page).scrollIntoViewIfNeeded();
  await page.waitForTimeout(400); // a dialog's rise has ended
  await page.screenshot({ path: join(SHOTS, `sfi-backend-${name}.png`) });
}
type Mock = Record<string, (...x: unknown[]) => unknown>;
const mock = <T,>(page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(([f, a]) => (window as unknown as { __kilnMock: Mock }).__kilnMock[f as string](...(a as unknown[])), [fn, args] as const) as Promise<T>;
const sf = async (page: Page, gameId = "client-steam") =>
  (await mock<{ shroudforge?: Record<string, unknown> }>(page, "kilnJsonValues", gameId)).shroudforge;
const sets = async (page: Page) => (await mock<string[]>(page, "calls")).filter((c) => c === "sfi_backend_set").length;

async function open(page: Page) {
  await page.goto("/#/settings/game");
  await page.locator(".k-splash").waitFor({ state: "detached" });
}
/** Leaves the page and comes back (the mock keeps its state): the page reads again. */
async function again(page: Page) {
  await page.evaluate(() => { location.hash = "/about"; });
  await page.waitForTimeout(150);
  await page.evaluate(() => { location.hash = "/settings/game"; });
  await expect(card(page)).toBeVisible();
}
const card = (page: Page) => page.getByRole("region", { name: "ShroudForge", exact: true });
const row = (page: Page, b: string) => card(page).getByTestId(`sf-backend-${b}`);
const dialog = (page: Page) => page.getByRole("dialog", { name: "Change the ShroudForge backend?" });

test("default, then the older switch, then a choice that replaces it (one key written)", async ({ page }) => {
  await open(page);
  const c = card(page);
  await expect(c.getByTestId("sf-backend-setting")).toHaveText("Nothing set: the default.");
  await expect(c.getByTestId("sf-backend-next")).toHaveAttribute("data-selected", "hosted");
  await expect(row(page, "hosted")).toHaveAttribute("aria-checked", "true");
  // the kiln.json card no longer shows the older switch: this card has it
  await expect(page.getByText("Use my installed ShroudForge instead of Kiln's bundled one")).toHaveCount(0);
  await shot(page, "default");

  await mock(page, "setKilnJson", "shroudforge.useInstalled", true);
  await again(page);
  await expect(c.getByTestId("sf-backend-setting")).toContainText('From the older switch "Use my installed ShroudForge": My installed ShroudForge.');
  await expect(row(page, "installed")).toHaveAttribute("aria-checked", "true");
  await expect(c.getByTestId("sf-backend-now")).toHaveAttribute("data-setting", "inherited");
  await shot(page, "inherited");

  await row(page, "hosted").click();
  const d = dialog(page);
  await expect(d).toBeVisible();
  await expect(d).toContainText("This replaces the older switch");
  await expect(d.getByTestId("sf-backend-ask-next")).toHaveAttribute("data-selected", "hosted");
  await shot(page, "ask-replaces");
  await d.getByRole("button", { name: "Save" }).click();
  await expect(d).toHaveCount(0);
  await expect.poll(() => sf(page)).toEqual({ backend: "hosted" });
  await expect(c.getByTestId("sf-backend-setting")).toHaveText("Chosen: Kiln's bundled ShroudForge.");
  await expect(c.getByTestId("sf-backend-now")).toHaveAttribute("data-setting", "explicit");
  await shot(page, "explicit");

  // back to the default: the key goes
  await c.getByRole("button", { name: "Back to the default" }).click();
  await dialog(page).getByRole("button", { name: "Save" }).click();
  await expect.poll(() => sf(page)).toBe(undefined);
  await expect(c.getByTestId("sf-backend-setting")).toHaveText("Nothing set: the default.");
});

test("Cancel and Esc write nothing", async ({ page }) => {
  await open(page);
  await row(page, "installed").click();
  await expect(dialog(page).getByTestId("sf-backend-ask-next")).toHaveAttribute("data-selected", "installed");
  await dialog(page).getByRole("button", { name: "Cancel" }).click();
  await expect(dialog(page)).toHaveCount(0);
  await row(page, "installed").click();
  await expect(dialog(page)).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(dialog(page)).toHaveCount(0);
  expect(await sets(page)).toBe(0);
  expect(await sf(page)).toBe(undefined);
  await expect(row(page, "hosted")).toHaveAttribute("aria-checked", "true");
});

test("Kiln's interpreter: greyed with its reason while held, never ShroudForge 1.8, chosen once lifted", async ({ page }) => {
  await open(page);
  const k = row(page, "kiln");
  await expect(k).toHaveAttribute("aria-disabled", "true");
  await expect(k.getByTestId("sf-backend-held")).toContainText("Not available in this Kiln build: game test pending: sfi-backend-roundtrip");
  await k.click({ force: true });
  await expect(dialog(page)).toHaveCount(0);
  await expect(card(page)).not.toContainText(/sf18|1\.8/);
  await expect(card(page).getByRole("radio")).toHaveCount(3);
  await shot(page, "kiln-held");

  // a hand-written "kiln" while held: refused at the next start, with the reason
  await mock(page, "setKilnJson", "shroudforge.backend", "kiln");
  await again(page);
  await expect(card(page).getByTestId("sf-backend-next")).toHaveAttribute("data-selected", "none");
  await expect(card(page).getByTestId("sf-backend-next")).toContainText("is held (game test pending");
  await shot(page, "kiln-refused");

  // back to the default from it (kiln is held, the default is not)
  await card(page).getByRole("button", { name: "Back to the default" }).click();
  await expect(dialog(page).getByTestId("sf-backend-ask-next")).toHaveAttribute("data-selected", "hosted");
  await dialog(page).getByRole("button", { name: "Save" }).click();
  await expect.poll(() => sf(page)).toBe(undefined);

  // kiln.json "preview" lifts the hold (a developer's switch); ShroudForge 1.8 still is no choice
  await mock(page, "setKilnJson", "preview", ["kiln-sfi.interpreter", "kiln-sfi.sf18"]);
  await again(page);
  await expect(row(page, "kiln")).toHaveAttribute("aria-disabled", "false");
  await expect(card(page)).not.toContainText(/sf18|1\.8/);
  await row(page, "kiln").click();
  const d = dialog(page);
  await expect(d.getByTestId("sf-backend-kiln-warning")).toContainText("Nothing switches back to Kiln's bundled ShroudForge on its own.");
  await shot(page, "ask-kiln");
  await d.getByRole("button", { name: "Save" }).click();
  await expect.poll(() => sf(page)).toEqual({ backend: "kiln" });
  await expect(card(page).getByTestId("sf-backend-next")).toHaveAttribute("data-selected", "kiln");
  // what a selected kiln meets in the game folder
  await mock(page, "setSfRoute", "missing");
  await again(page);
  await expect(card(page).getByTestId("sf-backend-route")).toContainText("Kiln's interpreter is not in this game yet");
  await mock(page, "setSfRoute", "own-loader");
  await again(page);
  await expect(card(page).getByTestId("sf-backend-route")).toContainText("ShroudForge's own loader is in the game (dbghelp.dll)");
  await shot(page, "kiln-own-loader");
});

test("an invalid value, the last start, and a held kiln that cannot be written", async ({ page }) => {
  await open(page);
  await mock(page, "setKilnJson", "shroudforge.backend", "sf18");
  await mock(page, "setSfLast", { backend: null, reasonClass: "refused", reason: "kiln.json shroudforge.backend: \"sf18\" is not \"hosted\", \"installed\" or \"kiln\"", pid: 4242 });
  await again(page);
  const c = card(page);
  await expect(c.getByTestId("sf-backend-setting")).toContainText("kiln.json names a backend Kiln does not know");
  await expect(c.getByRole("radio", { checked: true })).toHaveCount(0);
  await expect(c.getByTestId("sf-backend-last")).toContainText("Last start: no ShroudForge");
  // ShroudForge 1.8 appears only as the value the file holds, never as a choice
  await expect(c.getByRole("radio").filter({ hasText: /sf18|1\.8/ })).toHaveCount(0);
  await shot(page, "invalid");
  await row(page, "installed").click();
  await dialog(page).getByRole("button", { name: "Save" }).click();
  await expect.poll(() => sf(page)).toEqual({ backend: "installed" });
});

test("a late answer for another game never saves", async ({ page }) => {
  await open(page);
  const other = await mock<string>(page, "addSfGame", "/games/Enshrouded-2");
  await again(page);
  await mock(page, "planCommand", "sfi_backend_preview", { gate: "late" });
  await row(page, "installed").click();
  const d = dialog(page);
  await expect(d).toBeVisible();
  await expect(d.getByRole("button", { name: "Save" })).toBeDisabled();
  // the card switches to the other game (Settings' default game) while the answer for this one is on its way
  await page.locator("#st-game").selectOption(other);
  await expect(d).toHaveCount(0);
  await mock(page, "openGate", "late");
  await page.waitForTimeout(400);
  expect(await sets(page)).toBe(0);
  expect(await sf(page)).toBe(undefined);
  expect(await sf(page, other)).toBe(undefined);
  await expect(row(page, "hosted")).toHaveAttribute("aria-checked", "true");
  await expect(dialog(page)).toHaveCount(0);
});

test("kiln.json changed after the card read it: Save is refused and nothing is written", async ({ page }) => {
  await open(page);
  await row(page, "installed").click();
  await expect(dialog(page).getByTestId("sf-backend-ask-next")).toHaveAttribute("data-selected", "installed");
  await mock(page, "setKilnJson", "log_level", "debug");
  await dialog(page).getByRole("button", { name: "Save" }).click();
  await expect(page.getByText("kiln.json changed since the ShroudForge card read it")).toBeVisible();
  expect(await sf(page)).toBe(undefined);
  await expect(dialog(page)).toHaveCount(0);
});

test("kiln.json changed before the choice (another card, a hand edit): the dialog asks again on the bytes there now", async ({ page }) => {
  await open(page);
  await expect(row(page, "hosted")).toHaveAttribute("aria-checked", "true");
  // after the card read kiln.json, another key changes (the kiln.json card on this page, or a hand edit)
  await mock(page, "setKilnJson", "log_level", "debug");
  await row(page, "installed").click();
  const d = dialog(page);
  await expect(d.getByTestId("sf-backend-ask-next")).toHaveAttribute("data-selected", "installed");
  // Save is not left greyed with no way out: the card reads the file again and the preview is made on those bytes
  await expect(d.getByRole("button", { name: "Save" })).toBeEnabled();
  await d.getByRole("button", { name: "Save" }).click();
  await expect(d).toHaveCount(0);
  await expect.poll(() => sf(page)).toEqual({ backend: "installed" });
  expect((await mock<Record<string, unknown>>(page, "kilnJsonValues", "client-steam")).log_level).toBe("debug");
});

test("the running game locks the card", async ({ page }) => {
  await open(page);
  await mock(page, "externalStart", "client-steam");
  await expect(card(page).getByTestId("sf-backend-running")).toHaveText("Close the game to change this.");
  for (const b of ["hosted", "installed", "kiln"]) await expect(row(page, b)).toHaveAttribute("aria-disabled", "true");
  await row(page, "installed").click({ force: true });
  await expect(dialog(page)).toHaveCount(0);
  await shot(page, "running");
  await mock(page, "stopGame", "client-steam");
});

test("no backend picker on Play or in the mods list", async ({ page }) => {
  for (const hash of ["/", "/mods"]) {
    await page.goto(`/#${hash}`);
    await page.locator(".k-splash").waitFor({ state: "detached" });
    await page.waitForTimeout(300);
    await expect(page.getByRole("radiogroup", { name: "ShroudForge backend" })).toHaveCount(0);
    await expect(page.getByText("Kiln's interpreter")).toHaveCount(0);
  }
});
