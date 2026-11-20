// Inbox 89 (29.18): Play while a profile switch was still copying files. Play now says "Applying profile…" and stays
// off while any activation runs, whatever started it: the task-based ones (library_add, modpack_install, found_add,
// runtime_install, runtime_rollback, runtime_uninstall), read from the task list, and the guarded commands (a Load in
// Settings), on the
// desktop and the Deck; it comes back when the activation ends or fails. The mock holds a task kind before its last
// step until a test opens the gate (`__kilnMock.holdTasks`).
import { expect, test, type Page } from "@playwright/test";

type Mock = { __kilnMock: {
  holdTasks: (kind: string, hold: { gate: string; fail?: string } | null) => void;
  openGate: (name: string) => void;
  planCommand: (cmd: string, plan: { gate?: string; delay?: number }) => void;
} };
/** Tasks of `kind` wait before their last step until `openGate(gate)`; with `fail` they then fail. */
const hold = (page: Page, kind: string, gate: string, fail?: string) =>
  page.evaluate(([k, g, f]) => (window as unknown as Mock).__kilnMock.holdTasks(k, { gate: g, ...(f ? { fail: f } : {}) }), [kind, gate, fail ?? ""] as const);
const openGate = (page: Page, gate: string) => page.evaluate((g) => (window as unknown as Mock).__kilnMock.openGate(g), gate);

const G = "client-steam";
/** Each task-based activation, started straight through the API (as a drop, the Catalog, a modpack offer, Runtime or
 *  the mods list's Found rows would): the state comes from the task list, not from who started it. */
const KINDS: { kind: string; title: string }[] = [
  { kind: "library_add", title: "Add night-owl-1.2.0.zip" },
  { kind: "modpack_install", title: "Installing modpack ember-modpack.zip" },
  { kind: "found_add", title: "Add pack-mule" },
  { kind: "runtime_install", title: "Install Kiln 0.2.0-dev" },
  { kind: "runtime_rollback", title: "Roll back to 0.1.9+20261101.a41c2d0" },
];

async function startTask(page: Page, kind: string) {
  await page.evaluate(async ([k, g]) => {
    const api = await import("/src/api/commands.ts");
    if (k === "library_add") await api.libraryAdd(g, ["night-owl-1.2.0.zip"]);
    else if (k === "modpack_install") await api.modpackInstall(g, { source: { path: "ember-modpack.zip" }, consent: true, activate: true });
    else if (k === "found_add") await api.foundAdd(g, "ember-run", "mods/pack-mule");
    else if (k === "runtime_install") await api.runtimeInstall(g, "0.2.0-dev+20261104.0f1ffa6", {});
    else if (k === "runtime_rollback") await api.runtimeRollback(g, "0.1.9+20261101.a41c2d0");
    else if (k === "runtime_uninstall") await api.runtimeUninstall(g);
    else throw new Error(`no start for ${k}`);
  }, [kind, G] as const);
}

async function desktop(page: Page) {
  await page.goto("/#/");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.locator(".k-door")).toHaveAccessibleName("Play Ember run", { timeout: 10_000 });
}

async function deck(page: Page) {
  await page.goto("/?deck=steamdeck#/");
  await expect(page.locator('[data-deck-screen="home"]')).toBeVisible();
  await expect(page.locator(".deck-mouth")).toHaveAccessibleName("Play", { timeout: 10_000 });
  await expect(page.locator(".deck-mouth")).not.toHaveAttribute("aria-disabled", "true");
}

test.describe("desktop", () => {
  for (const k of KINDS) {
    test(`Play waits while ${k.kind} applies the profile, and comes back after it`, async ({ page }) => {
      await desktop(page);
      await hold(page, k.kind, `slow-${k.kind}`);
      await startTask(page, k.kind);
      const mouth = page.locator(".k-door");
      const what = k.kind.startsWith("runtime_") ? "Installing Kiln…" : "Applying profile…";
      await expect(mouth).toBeDisabled();
      await expect(mouth).toHaveAccessibleName(`${what} (${k.title})`);
      await expect(mouth.locator(".k-door-sub")).toHaveText(what);
      await expect(mouth.locator(".k-door-word")).toHaveText("Play");
      // the other ways to start wait too
      await expect(page.getByRole("button", { name: "Without mods" })).toBeDisabled();
      await openGate(page, `slow-${k.kind}`);
      await expect(mouth).toBeEnabled({ timeout: 8000 });
      await expect(mouth.locator(".k-door-sub")).toHaveCount(0);
    });
  }

  test("Kiln's uninstall holds Play with Removing Kiln…, then Install Kiln is offered", async ({ page }) => {
    await desktop(page);
    await hold(page, "runtime_uninstall", "slow-uninstall");
    await startTask(page, "runtime_uninstall");
    const mouth = page.locator(".k-door");
    await expect(mouth).toBeDisabled();
    await expect(mouth).toHaveAccessibleName("Removing Kiln… (Uninstall Kiln)");
    await expect(mouth.locator(".k-door-sub")).toHaveText("Removing Kiln…");
    await expect(page.getByRole("button", { name: "Without mods" })).toBeDisabled();
    await openGate(page, "slow-uninstall");
    await expect(mouth).toHaveAccessibleName("Install Kiln", { timeout: 8000 });
    await expect(mouth).toBeEnabled();
  });

  test("a dialog opened before an activation began does not start the game while it runs", async ({ page }) => {
    await desktop(page);
    await page.getByRole("button", { name: "Vanilla", exact: true }).click();
    const dialog = page.getByRole("dialog", { name: "Play vanilla once" });
    await expect(dialog).toBeVisible();
    await hold(page, "library_add", "slow-under-dialog");
    await startTask(page, "library_add");
    await expect(page.locator(".k-door")).toBeDisabled();
    await dialog.getByRole("button", { name: "Play vanilla" }).click();
    await expect(page.locator(".k-toast", { hasText: "Kiln is still loading a profile in this game" })).toBeVisible();
    await expect(page.locator(".k-door")).toHaveAccessibleName("Applying profile… (Add night-owl-1.2.0.zip)");
    await openGate(page, "slow-under-dialog");
    await expect(page.locator(".k-door")).toBeEnabled({ timeout: 8000 });
  });

  test("a failed activation gives Play back too", async ({ page }) => {
    await desktop(page);
    await hold(page, "library_add", "slow-fail", "The disk is full.");
    await startTask(page, "library_add");
    const mouth = page.locator(".k-door");
    await expect(mouth).toBeDisabled();
    await expect(mouth).toHaveAccessibleName("Applying profile… (Add night-owl-1.2.0.zip)");
    await openGate(page, "slow-fail");
    await expect(mouth).toBeEnabled({ timeout: 8000 });
    await expect(mouth).toHaveAccessibleName("Play Ember run");
  });

  test("a Load in Settings holds Play until it is done, on every page", async ({ page }) => {
    await desktop(page);
    await page.evaluate(() => (window as unknown as Mock).__kilnMock.planCommand("profile_activate", { gate: "slow-load" }));
    await page.evaluate(() => { location.hash = "#/settings"; });
    const row = page.getByRole("region", { name: "Profiles" }).getByRole("listitem").filter({ hasText: "Vanilla plus" });
    await row.getByRole("button", { name: "Load" }).click();
    // the pilot light in the corner is the same control
    const mouth = page.locator(".k-door");
    await expect(mouth).toBeDisabled();
    await expect(mouth).toHaveAccessibleName("Applying profile…");
    await page.evaluate(() => { location.hash = "#/"; });
    await expect(mouth).toBeDisabled();
    await expect(mouth.locator(".k-door-sub")).toHaveText("Applying profile…");
    await openGate(page, "slow-load");
    await expect(mouth).toHaveAccessibleName("Play Vanilla plus", { timeout: 8000 });
    await expect(mouth).toBeEnabled();
  });

  test("a drop that adds a mod holds Play while it is written to the game", async ({ page }) => {
    await desktop(page);
    await hold(page, "library_add", "slow-drop");
    const dt = await page.evaluateHandle(() => { const d = new DataTransfer(); d.items.add(new File(["x"], "night-owl-1.2.0.zip")); return d; });
    await page.locator(".k-hero").dispatchEvent("drop", { dataTransfer: dt });
    const mouth = page.locator(".k-door");
    await expect(mouth).toBeDisabled();
    await expect(mouth).toHaveAccessibleName("Applying profile… (Add night-owl-1.2.0.zip)");
    await openGate(page, "slow-drop");
    await expect(page.locator(".k-toast", { hasText: "Added Night Owl" })).toBeVisible({ timeout: 8000 });
    await expect(mouth).toBeEnabled();
  });
});

test.describe("deck", () => {
  test.use({ viewport: { width: 1280, height: 800 } });
  for (const k of KINDS) {
    test(`Home's Play waits while ${k.kind} applies the profile, and comes back after it`, async ({ page }) => {
      await deck(page);
      await hold(page, k.kind, `slow-${k.kind}`);
      await startTask(page, k.kind);
      const mouth = page.locator(".deck-mouth");
      const what = k.kind.startsWith("runtime_") ? "Installing Kiln…" : "Applying profile…";
      await expect(mouth).toHaveAttribute("aria-disabled", "true");
      await expect(mouth).toHaveAccessibleName(`Play: ${what}`);
      // it keeps the focus for the gamepad, and A starts nothing
      await expect.poll(() => page.evaluate(() => (document.activeElement as HTMLElement | null)?.dataset?.deckId ?? null)).toBe("mouth");
      await page.keyboard.press("Enter");
      await expect(page.locator(".deck-mouth")).toHaveAccessibleName(`Play: ${what}`);
      await openGate(page, `slow-${k.kind}`);
      await expect(mouth).not.toHaveAttribute("aria-disabled", "true", { timeout: 8000 });
      await expect(mouth).toHaveAccessibleName("Play");
    });
  }

  test("Kiln's uninstall holds Home's Play, which keeps the focus, then Install is offered", async ({ page }) => {
    await deck(page);
    await hold(page, "runtime_uninstall", "deck-uninstall");
    await startTask(page, "runtime_uninstall");
    const mouth = page.locator(".deck-mouth");
    await expect(mouth).toHaveAttribute("aria-disabled", "true");
    await expect(mouth).toHaveAccessibleName("Play: Removing Kiln…");
    await expect.poll(() => page.evaluate(() => (document.activeElement as HTMLElement | null)?.dataset?.deckId ?? null)).toBe("mouth");
    await openGate(page, "deck-uninstall");
    await expect(mouth).toHaveAccessibleName(/^Install/, { timeout: 8000 });
    await expect(mouth).not.toHaveAttribute("aria-disabled", "true");
  });

  test("Home's Play keeps the focus after an install's task ended and before its change has", async ({ page }) => {
    await deck(page);
    const focused = () => page.evaluate(() => (document.activeElement as HTMLElement | null)?.dataset?.deckId ?? null);
    await expect.poll(focused).toBe("mouth");
    // shell/runtimeChange.ts marks the change done a moment after the task's end event: the mouth waits, focused
    const put = (state: "running" | "done") => page.evaluate(async (st) => {
      const { useRuntimeChanges } = await import("/src/api/hooks.ts");
      useRuntimeChanges.getState().put("client-steam", { what: "Update to 0.2.0-dev+20261104.0f1ffa6", version: "0.2.0-dev+20261104.0f1ffa6", taskId: null, state: st, error: null, at: Date.now() });
    }, state);
    await put("running");
    const mouth = page.locator(".deck-mouth");
    await expect(mouth).toHaveAttribute("aria-disabled", "true");
    await expect(mouth).toHaveJSProperty("disabled", false);
    await expect.poll(focused).toBe("mouth");
    await put("done");
    await expect(mouth).not.toHaveAttribute("aria-disabled", "true");
    await expect.poll(focused).toBe("mouth");
  });

  test("a failed activation gives Home's Play back too", async ({ page }) => {
    await deck(page);
    await hold(page, "found_add", "deck-fail", "The disk is full.");
    await startTask(page, "found_add");
    const mouth = page.locator(".deck-mouth");
    await expect(mouth).toHaveAttribute("aria-disabled", "true");
    await openGate(page, "deck-fail");
    await expect(mouth).not.toHaveAttribute("aria-disabled", "true", { timeout: 8000 });
  });
});
