// Crucible on the Deck (src/crucible/DeckCrucible.tsx; DESIGN 2) at 1280×800 in mock mode, driven by the pad: Share
// to Crucible on Home (the first share's confirm, then the upload, then the link's QR code and the big code), Enter a
// code (Open), the preview with focus on Install and play, its check line, the trust row, and the install.
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";

test.use({ viewport: { width: 1280, height: 800 } });
const SHOTS = process.env.KILN_CRUCIBLE_SHOTS;
const shot = async (page: Page, name: string) => { if (SHOTS) { mkdirSync(SHOTS, { recursive: true }); await page.screenshot({ path: `${SHOTS}/${name}.png` }); } };

async function pad(page: Page, button: string) {
  const send = (pressed: boolean) => page.evaluate(([b, p]) => (window as unknown as { __kilnMock: { emit: (n: string, v: unknown) => void } }).__kilnMock
    .emit("kiln://gamepad", { kind: "button", button: b, pressed: p, repeat: false, device: "event9" }), [button, pressed] as const);
  await send(true);
  await send(false);
  await page.waitForTimeout(80);
}
const focused = (page: Page) => page.evaluate(() => (document.activeElement as HTMLElement | null)?.dataset?.deckId ?? null);
async function focusOn(page: Page, id: string) {
  await page.locator(`[data-deck-id="${id}"]`).waitFor();
  await page.locator(`[data-deck-id="${id}"]`).focus();
  await expect.poll(() => focused(page)).toBe(id);
}

async function openDeck(page: Page, url: string) {
  await page.goto(url);
  await expect(page.locator('[data-deck-screen="home"]')).toBeVisible();
  await expect.poll(() => focused(page)).toBe("mouth");
}

test("Share to Crucible: the confirm, then the QR code and the big code", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck&discord=in#/");
  await focusOn(page, "crucible-share");
  await pad(page, "a");
  const confirm = page.getByTestId("deck-crucible-confirm");
  await expect(confirm).toContainText("Goes up: the mod list");
  await expect(confirm).toContainText("Never sent: your saves, worlds, characters or any other personal file.");
  await shot(page, "deck-share-confirm");
  await page.waitForTimeout(300);
  expect(await page.evaluate(() => (window as unknown as { __kilnCrucibleMock: { requests: () => { cmd: string }[] } }).__kilnCrucibleMock.requests().filter((r) => r.cmd === "crucible_share").length)).toBe(0);
  await focusOn(page, "crucible-share-confirm");
  await pad(page, "a");
  await expect(page.getByTestId("deck-crucible-uploading")).toBeVisible();
  await expect(page.getByTestId("deck-crucible-code")).toHaveText("H7QK-3MZP", { timeout: 15_000 });
  await expect(page.locator('[data-deck-dialog="crucible-share"] svg')).toBeVisible();
  await shot(page, "deck-share");
  await pad(page, "b");
  await expect(page.locator('[data-deck-dialog="crucible-share"]')).toHaveCount(0);
});

test("Share signed out: confirm uploads without a Discord sign-in", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck#/");
  await focusOn(page, "crucible-share");
  await pad(page, "a");
  await expect(page.getByTestId("crucible-share-signin-first")).toHaveCount(0);
  await expect(page.getByTestId("deck-crucible-confirm")).toContainText("Anyone with the link or code can install");
  await focusOn(page, "crucible-share-confirm");
  await expect(page.locator('[data-deck-id="crucible-share-confirm"]')).toHaveText("Share");
  await pad(page, "a");
  await expect(page.getByTestId("deck-crucible-code")).toHaveText("H7QK-3MZP", { timeout: 15_000 });
  await expect(page.getByTestId("deck-qr")).toHaveCount(0);
});

test("an anonymous Deck link updates signed out on the same code", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck&crucible=shared#/");
  await focusOn(page, "crucible-share");
  await pad(page, "a");
  const dialog = page.locator('[data-deck-dialog="crucible-share"]');
  await expect(dialog).toContainText("Update link");
  await dialog.getByRole("button", { name: "Update link" }).click();
  await expect(page.getByTestId("deck-crucible-code")).toHaveText("H7QK-3MZP", { timeout: 15_000 });
  await expect(page.getByTestId("deck-qr")).toHaveCount(0);
  const shared = await page.evaluate(async () => (await import("/src/api/commands.ts")).crucibleShared("ember-run"));
  expect(shared?.version.number).toBe(2);
  expect(shared?.requiresSignIn).toBe(false);
});

test("account-owned Deck updates still offer Discord sign-in", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck&crucible=shared,accountshare#/");
  await focusOn(page, "crucible-share");
  await pad(page, "a");
  const dialog = page.locator('[data-deck-dialog="crucible-share"]');
  await expect(dialog.getByRole("button", { name: "Sign in with Discord" })).toBeVisible();
  await expect(dialog.getByRole("button", { name: "Update link" })).toHaveCount(0);
});

test("Enter a code: the preview with focus on Install and play, trust, install", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck#/");
  await focusOn(page, "crucible-code");
  await pad(page, "a");
  const input = page.getByTestId("deck-crucible-code-input");
  await expect(input).toBeFocused();
  await input.pressSequentially("k4tw9xrb");
  await expect(input).toHaveValue("K4TW-9XRB");
  await input.press("Enter");
  await expect(page.getByTestId("deck-crucible-preview")).toContainText("by Cinderhawk");
  await expect(page.getByTestId("deck-crucible-checks")).toHaveText("Checks: passed");
  await expect.poll(() => focused(page)).toBe("crucible-install");
  await expect(page.getByTestId("deck-crucible-warning")).toContainText("This pack contains mods, including code, from other people.");
  await shot(page, "deck-preview");
  // not ticked yet: A says what is missing
  await pad(page, "a");
  await expect(page.getByTestId("deck-crucible-note")).toHaveText('Tick "I trust" first.');
  await focusOn(page, "crucible-trust");
  await pad(page, "a");
  await focusOn(page, "crucible-install");
  await pad(page, "a");
  await expect(page.locator('[data-deck-dialog="crucible-done"]')).toContainText("Cinder Trails (Cinderhawk's pack) is installed", { timeout: 15_000 });
  await shot(page, "deck-installed");
  await focusOn(page, "crucible-undo");
  await pad(page, "a");
  await expect(page.locator('[data-deck-dialog="crucible-done"]')).toHaveCount(0);
  await expect(page.getByTestId("deck-profile")).toContainText("Ember run");
});

test("a kiln:// link opens the Deck preview", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck#/");
  await page.evaluate(() => (window as unknown as { __kilnCrucibleMock: { link: (u: string) => void } }).__kilnCrucibleMock.link("https://frostdev.io/kiln/crucible/p/K4TW-9XRB"));
  await expect(page.locator('[data-deck-dialog="crucible-preview"]')).toContainText("Cinder Trails");
});

const calls = (page: Page) => page.evaluate(() => (window as unknown as { __kilnMock: { calls: () => string[] } }).__kilnMock.calls());
const packRequests = (page: Page) => page.evaluate(() => (window as unknown as { __kilnCrucibleMock: { requests: () => { cmd: string; args: unknown }[] } }).__kilnCrucibleMock.requests());

async function openExportFromShare(page: Page) {
  await focusOn(page, "crucible-share");
  await pad(page, "a");
  await expect.poll(() => focused(page)).toBe("crucible-share-confirm");
  await pad(page, "left");
  await expect.poll(() => focused(page)).toBe("crucible-save-file");
  await pad(page, "a");
  await expect(page.locator('[data-deck-dialog="pack-export"]')).toBeVisible();
  await expect(page.getByTestId("deck-export-destination")).toHaveText("/home/player/Downloads");
}

for (const community of ["", "&discord=offline", "&discord=relaydown", "&discord=hidden"]) test(`local export before QR is signed-out, offline and Community-independent (${community || "signed out"})`, async ({ page }) => {
  await openDeck(page, `/?deck=steamdeck${community}#/`);
  await openExportFromShare(page);
  await expect.poll(() => focused(page)).toBe("pack-export-confirm");
  await expect(page.locator('[data-deck-dialog="pack-export"]')).toContainText("Nothing is uploaded");
  await shot(page, "deck-export-confirm-" + (community.replace(/[^a-z]/g, "") || "signedout"));
  // Even two starts in one event loop turn may start only one build.
  await page.locator('[data-deck-id="pack-export-confirm"]').evaluate((el: HTMLButtonElement) => { el.click(); el.click(); });
  await expect(page.getByTestId("deck-export-file")).toHaveText("/home/player/Downloads/ember-run-1.0.0.kilnpack");
  const invoked = await calls(page);
  expect(invoked.filter((x) => x === "pack_build")).toHaveLength(1);
  expect(invoked).not.toContain("community_signin_start");
  expect(invoked).not.toContain("crucible_share");
  expect((await packRequests(page)).find((r) => r.cmd === "pack_build")?.args).toEqual({ request: { profile: "ember-run", name: "Ember run", version: "1.0.0" }, outDir: "/home/player/Downloads" });
  await shot(page, "deck-export-saved-" + (community.replace(/[^a-z]/g, "") || "signedout"));
});

test("local export is reachable after Back from failed QR, without another sign-in or share", async ({ page }) => {
  // New links need no account; only a legacy account-owned update still enters the QR flow.
  await openDeck(page, "/?deck=steamdeck&crucible=shared,accountshare&discord=relaydown#/");
  await focusOn(page, "crucible-share"); await pad(page, "a");
  const signin = page.locator('[data-deck-dialog="crucible-share"]').getByRole("button", { name: "Sign in with Discord" });
  await signin.focus();
  await expect(signin).toBeFocused();
  await pad(page, "a");
  await expect(page.getByTestId("deck-qr")).toContainText("isn't answering");
  await pad(page, "b");
  await expect(page.getByTestId("deck-qr")).toHaveCount(0);
  const before = (await calls(page)).filter((x) => x === "community_signin_start").length;
  // The existing nested-dialog navigation may use the first move to seed focus after the QR is removed.
  for (let i = 0; i < 2 && await focused(page) !== "crucible-save-file"; i++) await pad(page, "left");
  await expect.poll(() => focused(page)).toBe("crucible-save-file");
  await pad(page, "a");
  await expect(page.getByTestId("deck-export-destination")).toBeVisible();
  await pad(page, "a");
  await expect(page.getByTestId("deck-export-file")).toBeVisible();
  const after = await calls(page);
  expect(after.filter((x) => x === "community_signin_start")).toHaveLength(before);
  expect(after.filter((x) => x === "pack_build")).toHaveLength(1);
  expect(after).not.toContain("crucible_share");
});

for (const exit of ["b", "cancel"]) test(`cancel export before confirmation writes nothing (${exit})`, async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck#/");
  await openExportFromShare(page);
  if (exit === "cancel") { await pad(page, "left"); await expect.poll(() => focused(page)).toBe("pack-export-cancel"); await pad(page, "a"); }
  else await pad(page, "b");
  await expect(page.locator('[data-deck-dialog="pack-export"]')).toHaveCount(0);
  await expect(page.locator(".deck-toasts")).toContainText("Export canceled");
  const invoked = await calls(page);
  for (const cmd of ["pack_build", "community_signin_start", "crucible_share", "app_dialog_remember"]) expect(invoked).not.toContain(cmd);
});

for (const status of ["off", "error"]) test(`Home local export does not depend on Crucible status (${status})`, async ({ page }) => {
  if (status === "error") await page.addInitScript(() => { (window as unknown as { __kilnMockPlans: unknown }).__kilnMockPlans = { crucible_status: [{ error: { code: "io", message: "Status unavailable" } }] }; });
  await openDeck(page, `/?deck=steamdeck&crucible=${status}#/`);
  if (status === "error") {
    // Refuse subsequent status reads too; this is a local query failure, not a health probe.
    await page.evaluate(async () => {
      const mock = (window as unknown as { __kilnMock: { planCommand: (c: string, p: unknown) => void; emit: (n: string, p: unknown) => void } }).__kilnMock;
      mock.planCommand("crucible_status", { error: { code: "io", message: "Status unavailable" } });
      mock.emit("kiln://community", { kind: "changed" });
    });
  }
  await expect(page.locator('[data-deck-id="crucible-share"]')).toHaveCount(0);
  await focusOn(page, "pack-export"); await pad(page, "a");
  await expect(page.getByTestId("deck-export-destination")).toBeVisible();
  await pad(page, "a");
  await expect(page.getByTestId("deck-export-file")).toBeVisible();
  expect((await calls(page)).filter((x) => x === "pack_build")).toHaveLength(1);
});

test("export shows build failure and cancellation, then retries only on request", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck#/");
  await openExportFromShare(page);
  await page.evaluate(() => {
    const mock = (window as unknown as { __kilnMock: { planCommand: (cmd: string, p: unknown) => void } }).__kilnMock;
    mock.planCommand("pack_build", { error: { code: "io", message: "Destination is read-only" } });
    mock.planCommand("pack_build", { error: { code: "canceled", message: "canceled" } });
  });
  await pad(page, "a");
  await expect(page.getByTestId("deck-export-error")).toContainText("Destination is read-only");
  await expect(page.getByTestId("deck-export-file")).toHaveCount(0);
  expect((await calls(page)).filter((x) => x === "pack_build")).toHaveLength(1);
  await pad(page, "a");
  await expect(page.getByTestId("deck-export-error")).toContainText("Export canceled");
  await pad(page, "a");
  await expect(page.getByTestId("deck-export-file")).toBeVisible();
  expect((await calls(page)).filter((x) => x === "pack_build")).toHaveLength(3);
});

test("an unresolved destination cannot write a pack", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck#/");
  await page.evaluate(() => {
    const mock = (window as unknown as { __kilnMock: { planCommand: (cmd: string, p: unknown) => void } }).__kilnMock;
    // Development StrictMode resolves the read-only destination in both effect mounts.
    for (let i = 0; i < 2; i++) mock.planCommand("app_dialog_default", { result: null });
  });
  await focusOn(page, "pack-export"); await pad(page, "a");
  await expect(page.getByTestId("deck-export-error")).toContainText("Downloads or home folder");
  await expect(page.locator('[data-deck-id="pack-export-confirm"]')).toBeDisabled();
  await pad(page, "a"); await pad(page, "b");
  expect(await calls(page)).not.toContain("pack_build");
});

for (const taskFailure of [false, true]) test(`export recovers changed Library files in the confirmed folder (${taskFailure ? "task" : "command"})`, async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck#/");
  await openExportFromShare(page);
  await page.evaluate(async (taskFailure) => {
    const api = await import("/src/api/commands.ts");
    const p = await api.profileGet("ember-run");
    const entry = { kind: "shroudforge", ...p.mods.shroudforge[0] };
    const mock = (window as unknown as { __kilnMock: { editInLibrary: (kind: string, id: string, c: unknown) => void; planCommand: (cmd: string, p: unknown) => void; emit: (n: string, p: unknown) => void } }).__kilnMock;
    mock.editInLibrary(entry.kind, entry.id, { name: "Better Chests edited", version: "0.4.2" });
    const error = { code: "hash_mismatch", message: "Better Chests was changed in its folder", detail: { libraryChanged: entry, name: "Better Chests" } };
    if (taskFailure) {
      mock.planCommand("pack_build", { result: "pack-library-error" });
      const { useTasks } = await import("/src/ui/TaskProgress.tsx");
      useTasks.getState().upsert({ id: "pack-library-error", kind: "pack_build", title: "Save pack", state: "failed", progress: null,
        message: error.message, started: new Date().toISOString(), finished: new Date().toISOString(), error });
    } else mock.planCommand("pack_build", { error });
  }, taskFailure);
  await pad(page, "a");
  await expect(page.getByTestId("deck-export-error")).toContainText("Better Chests was changed in its folder");
  await expect.poll(() => focused(page)).toBe("pack-export-recover");
  expect((await calls(page)).filter((x) => x === "pack_build")).toHaveLength(1);
  await pad(page, "a");
  await expect(page.getByTestId("deck-export-file")).toHaveText("/home/player/Downloads/ember-run-1.0.0.kilnpack");
  const now = await page.evaluate(async () => (await (await import("/src/api/commands.ts")).profileGet("ember-run")).mods.shroudforge[0]);
  expect(now.version).toBe("0.4.2");
  const invoked = await calls(page);
  expect(invoked.filter((x) => x === "library_take_over")).toHaveLength(1);
  expect(invoked.filter((x) => x === "pack_build")).toHaveLength(2);
  expect(invoked).not.toContain("community_signin_start");
  expect(invoked).not.toContain("crucible_share");
});

test("home fallback is explicit and Back during destination resolution cannot start a late write", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck#/");
  await page.evaluate(() => {
    const mock = (window as unknown as { __kilnMock: { planCommand: (cmd: string, p: unknown) => void } }).__kilnMock;
    for (let i = 0; i < 2; i++) mock.planCommand("app_dialog_default", { gate: "export-folder", result: "/home/player" });
  });
  await focusOn(page, "pack-export"); await pad(page, "a");
  await expect(page.locator('[data-deck-dialog="pack-export"]')).toContainText("Finding Downloads");
  await pad(page, "b");
  await page.evaluate(() => (window as unknown as { __kilnMock: { openGate: (name: string) => void } }).__kilnMock.openGate("export-folder"));
  await expect(page.locator('[data-deck-dialog="pack-export"]')).toHaveCount(0);
  expect(await calls(page)).not.toContain("pack_build");
  await page.evaluate(() => {
    const mock = (window as unknown as { __kilnMock: { planCommand: (cmd: string, p: unknown) => void } }).__kilnMock;
    for (let i = 0; i < 2; i++) mock.planCommand("app_dialog_default", { result: "/home/player" });
  });
  await focusOn(page, "pack-export"); await pad(page, "a");
  await expect(page.getByTestId("deck-export-destination")).toHaveText("/home/player");
  await expect.poll(() => focused(page)).toBe("pack-export-confirm");
  await pad(page, "a");
  await expect(page.getByTestId("deck-export-file")).toHaveText("/home/player/ember-run-1.0.0.kilnpack");
  expect((await calls(page)).filter((x) => x === "pack_build")).toHaveLength(1);
});

test("a running export cannot be dismissed or restarted with the pad", async ({ page }) => {
  await openDeck(page, "/?deck=steamdeck#/");
  await openExportFromShare(page);
  await page.evaluate(() => (window as unknown as { __kilnMock: { holdTasks: (kind: string, hold: unknown) => void } }).__kilnMock.holdTasks("pack_build", { gate: "export-writing" }));
  await pad(page, "a");
  await expect(page.locator('[data-deck-dialog="pack-export"]')).toContainText("Saving and checking");
  await pad(page, "b"); await pad(page, "a"); await pad(page, "a");
  await expect(page.locator('[data-deck-dialog="pack-export"]')).toBeVisible();
  expect((await calls(page)).filter((x) => x === "pack_build")).toHaveLength(1);
  await page.evaluate(() => (window as unknown as { __kilnMock: { openGate: (name: string) => void } }).__kilnMock.openGate("export-writing"));
  await expect(page.getByTestId("deck-export-file")).toBeVisible();
  await expect.poll(() => focused(page)).toBe("pack-export-done");
  await pad(page, "a");
  await expect(page.locator('[data-deck-dialog="crucible-share"]')).toBeVisible();
  expect((await calls(page)).filter((x) => x === "pack_build")).toHaveLength(1);
});
