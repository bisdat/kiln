// the warning before Play in mock mode: every route this launcher starts the game from
// (the shell's Play, Play once and vanilla through the shell's play control, the Deck's Home, a modpack offer's Launch)
// asks launch_play (the prepare: it opens nothing while warnings wait), shows the dialog and confirms exactly that token before anything opens; Cancel (its button, Enter on it,
// Escape, a click beside it, the Deck's B) opens nothing and ends the start; a changed set of warnings is a new dialog;
// launch_play itself refuses an unacknowledged start; folder notes off and notices already seen change nothing; a long
// list scrolls and stays reachable; the base the plan was made from is shown apart in its exact words.
// Screenshots go to .out/launcher/shots/ (SHOTS=0 skips them).
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";
import { join } from "node:path";

const SHOTS = join(import.meta.dirname, "..", "..", ".out", "launcher", "shots");
async function shot(page: Page, name: string) {
  if (process.env.SHOTS === "0") return;
  mkdirSync(SHOTS, { recursive: true });
  await page.waitForTimeout(400);
  await page.screenshot({ path: join(SHOTS, `sfi-play-${name}.png`) });
}
type Mock = Record<string, (...x: unknown[]) => unknown>;
const mock = <T,>(page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(([f, a]) => (window as unknown as { __kilnMock: Mock }).__kilnMock[f as string](...(a as unknown[])), [fn, args] as const) as Promise<T>;
const api = <T,>(page: Page, fn: string, ...args: unknown[]) =>
  page.evaluate(async ([f, a]) => {
    const m = (await import("/src/api/commands.ts")) as unknown as Record<string, (...x: unknown[]) => Promise<unknown>>;
    try { return { ok: await m[f as string](...(a as unknown[])) }; } catch (e) { return { err: e }; }
  }, [fn, args] as const) as Promise<{ ok?: T; err?: { code: string; message: string; detail?: { preflight?: { token: string; warnings: unknown[] } } } }>;
const calls = (page: Page) => mock<string[]>(page, "calls");
const checks = (page: Page) => mock<string[]>(page, "startChecks");
const running = (page: Page) => mock<boolean>(page, "gameRunning");

const BASE = "unclassified: game files as found when Kiln first managed this install";
const skipped = (id: string, o: Record<string, unknown> = {}) => ({
  kind: "package-skipped", code: "dependency-skipped", message: `${id} needs lib-b`, severity: "warning", causedBy: ["lib-b"],
  package: { id, version: "1.2.0", root: `mods/${id}`, saveSensitive: false, data: true, lua: true, dlls: [`${id}.dll`],
    reasons: [{ code: "dependency-skipped", message: `needs lib-b, which does not run`, severity: "warning", causedBy: ["lib-b"] }] }, ...o,
});
const WARNINGS = [
  skipped("needy"),
  { kind: "previously-active-off", code: "save-sensitive-now-off", message: "m", severity: "save-warning",
    previous: { id: "ammo", version: "0.8.2", name: "Quiver", basis: "policy", fromLaunchId: "00000000000000ab" } },
];
const ADVISORIES = [{ kind: "base-origin", code: "first-managed-unclassified", message: BASE, severity: "info" }];
const warn = (page: Page, w: unknown[] = WARNINGS, a: unknown[] = ADVISORIES) => mock(page, "setStartWarnings", w, a);

async function desktop(page: Page) {
  await page.goto("/#/");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.locator(".k-door")).toBeEnabled({ timeout: 10_000 });
}
const dialog = (page: Page) => page.getByRole("dialog", { name: "Some mods will not run this start" });
const play = (page: Page, mode: string, profileId?: string) =>
  page.evaluate(([m, p]) => (window as unknown as { __kilnPlay: (m: string, p?: string) => void }).__kilnPlay(m as string, p ?? undefined), [mode, profileId ?? null] as const);
/** Cancel ended the start: nothing opened, no confirm, the token dropped, the start's timing ended, Play back. */
async function canceled(page: Page, prepares: number) {
  await expect(dialog(page)).toHaveCount(0);
  await expect.poll(async () => (await checks(page)).map((c) => c.split(" ")[0])).toEqual(Array.from({ length: prepares }, () => ["play", "cancel"]).flat());
  // launch_play was the prepare (it opened nothing); no confirm
  expect((await calls(page)).filter((c) => c === "launch_confirm")).toEqual([]);
  expect(await running(page)).toBe(false);
  expect(await mock(page, "startTokens")).toBe(0);
  await expect.poll(async () => (await calls(page)).filter((c) => c === "launch_end").length).toBe(prepares);
  await expect(page.locator(".k-toast", { hasText: /canceled|error/i })).toHaveCount(0);
  await expect(page.locator(".k-door")).toBeEnabled({ timeout: 10_000 });
}

test("Play with warnings: the dialog lists them in full, Cancel has the focus, and Cancel, Enter, Escape or a click beside it open nothing", async ({ page }) => {
  await desktop(page);
  await warn(page);
  await page.locator(".k-door").click();
  const d = dialog(page);
  await expect(d).toBeVisible();
  await expect(d.getByTestId("sfi-warning-cancel")).toBeFocused();
  await expect(d.getByTestId("sfi-warning")).toHaveAttribute("data-lead", "save");
  await expect(d.getByTestId("sfi-warning-lead")).toContainText("If you save a world while it is off");
  const items = d.getByTestId("sfi-warning-item");
  await expect(items).toHaveCount(2);
  await expect(items.nth(0)).toContainText("needy 1.2.0 does not run");
  await expect(items.nth(0)).toContainText("Parts: startup data · Lua · DLLs: needy.dll");
  await expect(items.nth(0)).toContainText("Because of: lib-b");
  await expect(items.nth(0)).toContainText("Folder: mods/needy");
  await expect(items.nth(1)).toContainText("Quiver (ammo 0.8.2) is off now");
  await expect(items.nth(1)).toContainText("do not save a world while it is off");
  // the base, apart from the warnings, in its exact words, with Restore and Steam's Verify
  const notes = d.getByTestId("sfi-warning-notes");
  await expect(notes.getByText(BASE, { exact: true })).toBeVisible();
  await expect(notes).toContainText("start the game through Kiln once (Logs, Engines: Restore the game's data)");
  await expect(notes).toContainText("Verify integrity of game files");
  await expect(d.getByTestId("sfi-warning-list")).not.toContainText(BASE);
  // nothing was started while the dialog waits
  expect(await checks(page)).toEqual(["play client-steam profile"]);
  expect(await running(page)).toBe(false);
  await shot(page, "dialog");
  await d.getByTestId("sfi-warning-cancel").click();
  await canceled(page, 1);

  // Enter on the focused Cancel
  await page.locator(".k-door").click();
  await expect(dialog(page).getByTestId("sfi-warning-cancel")).toBeFocused();
  await page.keyboard.press("Enter");
  await canceled(page, 2);
  // Escape
  await page.locator(".k-door").click();
  await expect(dialog(page)).toBeVisible();
  await page.keyboard.press("Escape");
  await canceled(page, 3);
  // a click beside the dialog
  await page.locator(".k-door").click();
  await expect(dialog(page)).toBeVisible();
  await page.mouse.click(5, 5);
  await canceled(page, 4);
});

test("Continue without these mods opens the game exactly once, with the token the dialog showed", async ({ page }) => {
  await desktop(page);
  await warn(page);
  await page.locator(".k-door").click();
  await dialog(page).getByTestId("sfi-warning-continue").click();
  await expect(dialog(page)).toHaveCount(0);
  await expect.poll(() => running(page)).toBe(true);
  expect((await checks(page)).map((c) => c.split(" ")[0])).toEqual(["play", "confirm"]);
  expect((await calls(page)).filter((c) => c === "launch_confirm" || c === "launch_play")).toEqual(["launch_play", "launch_confirm"]);
  await expect(page.locator(".k-head-state")).toHaveText(/Starting|Running/, { timeout: 10_000 });
  await mock(page, "stopGame");
});

test("a start with nothing to warn about shows no dialog and starts at once (launch_play alone)", async ({ page }) => {
  await desktop(page);
  await page.locator(".k-door").click();
  await expect.poll(() => running(page)).toBe(true);
  expect((await checks(page)).map((c) => c.split(" ")[0])).toEqual(["play"]);
  expect((await calls(page)).filter((c) => c === "launch_confirm" || c === "launch_prepare")).toEqual([]);
  await expect(dialog(page)).toHaveCount(0);
  await mock(page, "stopGame");
});

test("warnings that change while the dialog is open come as a new dialog; the start is still timed in between", async ({ page }) => {
  await desktop(page);
  await warn(page);
  await page.locator(".k-door").click();
  await expect(dialog(page)).toBeVisible();
  await warn(page, [...WARNINGS, skipped("needy2")]);
  await dialog(page).getByTestId("sfi-warning-continue").click();
  const d = dialog(page);
  await expect(d.getByTestId("sfi-warning")).toHaveAttribute("data-round", "1");
  await expect(d).toContainText("changed since the last warning");
  await expect(d.getByTestId("sfi-warning-item")).toHaveCount(3);
  await expect(d.getByTestId("sfi-warning-cancel")).toBeFocused();
  expect(await running(page)).toBe(false);
  // one start: launch_begin once, no launch_end between the two dialogs
  expect((await calls(page)).filter((c) => c === "launch_begin" || c === "launch_end")).toEqual(["launch_begin"]);
  await shot(page, "changed");
  await d.getByTestId("sfi-warning-continue").click();
  await expect.poll(() => running(page)).toBe(true);
  expect((await checks(page)).map((c) => c.split(" ")[0])).toEqual(["play", "confirm", "confirm"]);
  expect((await calls(page)).filter((c) => c === "launch_end")).toEqual([]);
  await mock(page, "stopGame");
});

test("launch_play itself refuses an unacknowledged start and opens nothing", async ({ page }) => {
  await desktop(page);
  await warn(page);
  const r = await api(page, "launchPlay", "client-steam", { mode: "profile", profileId: null });
  expect(r.err?.code).toBe("consent_required");
  expect(r.err?.detail?.preflight?.warnings).toHaveLength(2);
  expect(await running(page)).toBe(false);
  // a token used once (here: confirmed with another kind of start) is gone; the next try prepares again
  const p = await api<{ token: string }>(page, "launchPrepare", "client-steam", { mode: "profile", profileId: null });
  const wrong = await api(page, "launchConfirm", "client-steam", p.ok!.token, { mode: "no-mods", profileId: null });
  expect(wrong.err?.code).toBe("refused");
  const again = await api(page, "launchConfirm", "client-steam", p.ok!.token, { mode: "profile", profileId: null });
  expect((again.err?.detail as unknown as { token: string }).token).toBe("unknown");
  expect(await running(page)).toBe(false);
});

test("folder notes off and notices already seen change nothing: every Play asks again", async ({ page }) => {
  await desktop(page);
  const s = await api<Record<string, unknown>>(page, "appSettingsGet");
  await api(page, "appSettingsSet", { ...s.ok, modFolderNotes: false });
  await api(page, "noticesShown", "client-steam", ["note:needy needs lib-b", "note:m"]);
  await warn(page);
  for (const n of [1, 2]) {
    await page.locator(".k-door").click();
    await expect(dialog(page)).toBeVisible();
    await dialog(page).getByTestId("sfi-warning-cancel").click();
    await canceled(page, n);
  }
});

test("Play once and vanilla through the shell's play control ask first too", async ({ page }) => {
  await desktop(page);
  await warn(page);
  await play(page, "once", "vanilla-plus");
  await expect(dialog(page)).toBeVisible();
  expect(await checks(page)).toEqual(["play client-steam once"]);
  await dialog(page).getByTestId("sfi-warning-cancel").click();
  await expect(dialog(page)).toHaveCount(0);
  await play(page, "vanilla");
  await expect(dialog(page)).toBeVisible();
  await dialog(page).getByTestId("sfi-warning-continue").click();
  await expect.poll(() => running(page)).toBe(true);
  expect(await checks(page)).toEqual(["play client-steam once", "cancel client-steam", "play client-steam vanilla", "confirm client-steam vanilla"]);
  await mock(page, "stopGame");
});

test("a modpack offer's Launch asks first: Cancel keeps the offer and opens nothing, Continue starts", async ({ page }) => {
  await desktop(page);
  await page.locator(".k-hero-banner .k-banner").getByRole("button", { name: "Get modpack" }).click();
  const offer = page.getByRole("dialog", { name: /uses mods$/ });
  await offer.getByText(/^I trust .* and want its mods$/).click();
  await offer.getByRole("button", { name: "Download and install" }).click();
  const installed = page.getByRole("dialog", { name: "Modpack installed" });
  await expect(installed).toBeVisible({ timeout: 8000 });
  await warn(page);
  await installed.getByRole("button", { name: "Launch" }).click();
  await expect(dialog(page)).toBeVisible({ timeout: 10_000 });
  await dialog(page).getByTestId("sfi-warning-cancel").click();
  await expect(dialog(page)).toHaveCount(0);
  await expect(installed).toBeVisible();
  expect(await running(page)).toBe(false);
  expect((await calls(page)).filter((c) => c === "launch_confirm")).toEqual([]);
  await expect(page.locator(".k-toast", { hasText: "could not" })).toHaveCount(0);
  await installed.getByRole("button", { name: "Launch" }).click();
  await dialog(page).getByTestId("sfi-warning-continue").click();
  await expect.poll(() => running(page)).toBe(true);
  await expect(installed).toHaveCount(0);
  await mock(page, "stopGame");
});

test("a long list scrolls, every warning stays in it in full, and the keyboard reaches it", async ({ page }) => {
  await desktop(page);
  await warn(page, Array.from({ length: 40 }, (_, i) => skipped(`pkg-${String(i).padStart(2, "0")}`, { message: "x".repeat(200) })), []);
  await page.locator(".k-door").click();
  const list = dialog(page).getByTestId("sfi-warning-list");
  await expect(list.getByTestId("sfi-warning-item")).toHaveCount(40);
  expect(await list.evaluate((el) => el.scrollHeight > el.clientHeight)).toBe(true);
  await expect(list).toHaveAttribute("tabindex", "0");
  await expect(list).toHaveAttribute("aria-label", "What will not run (40)");
  // Tab moves between the list and the two buttons and stays in the dialog
  const seen = new Set<string>();
  for (let i = 0; i < 4; i++) {
    await page.keyboard.press("Tab");
    seen.add(await page.evaluate(() => (document.activeElement as HTMLElement).dataset.testid ?? ""));
  }
  expect([...seen].sort()).toEqual(["sfi-warning-cancel", "sfi-warning-continue", "sfi-warning-list"]);
  await list.focus();
  await page.keyboard.press("End");
  await expect(list.getByTestId("sfi-warning-item").last()).toBeInViewport();
  await shot(page, "long");
  await page.keyboard.press("Escape");
  await canceled(page, 1);
});

test.describe("in the Deck view", () => {
  test.use({ viewport: { width: 1280, height: 800 } });
  const pad = async (page: Page, button: string) => {
    const send = (pressed: boolean) => page.evaluate(([b, p]) => (window as unknown as { __kilnMock: Mock }).__kilnMock
      .emit("kiln://gamepad", { kind: "button", button: b, pressed: p, repeat: false, device: "event9" }), [button, pressed] as const);
    await send(true);
    await send(false);
    await page.waitForTimeout(60);
  };
  const focusedText = (page: Page) => page.evaluate(() => document.activeElement?.textContent ?? "");

  test("Home's Play asks first: Cancel focused, B cancels, A on Continue starts once", async ({ page }) => {
    await page.goto("/?deck=steamdeck#/");
    await expect(page.locator('[data-deck-screen="home"]')).toBeVisible();
    await expect.poll(() => page.evaluate(() => (document.activeElement as HTMLElement | null)?.dataset?.deckId ?? null)).toBe("mouth");
    await warn(page);
    await pad(page, "a");
    const d = page.locator('[data-deck-dialog="sfi-warning"]');
    await expect(d).toBeVisible();
    await expect(d).toContainText("Quiver (ammo 0.8.2) is off now");
    await expect.poll(() => focusedText(page)).toBe("Cancel");
    await shot(page, "deck");
    await pad(page, "b");
    await expect(d).toHaveCount(0);
    expect(await running(page)).toBe(false);
    expect((await checks(page)).map((c) => c.split(" ")[0])).toEqual(["play", "cancel"]);
    // A on the focused Cancel cancels too
    await pad(page, "a");
    await expect(d).toBeVisible();
    await expect.poll(() => focusedText(page)).toBe("Cancel");
    await pad(page, "a");
    await expect(d).toHaveCount(0);
    expect(await running(page)).toBe(false);
    // right to Continue, then A
    await pad(page, "a");
    await expect(d).toBeVisible();
    await expect.poll(() => focusedText(page)).toBe("Cancel");
    await pad(page, "right");
    await expect.poll(() => focusedText(page)).toBe("Continue without these mods");
    await pad(page, "a");
    await expect.poll(() => running(page)).toBe(true);
    expect((await checks(page)).map((c) => c.split(" ")[0])).toEqual(["play", "cancel", "play", "cancel", "play", "confirm"]);
    await mock(page, "stopGame");
  });
});
