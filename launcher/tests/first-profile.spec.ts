// A selected game still points at another launcher's data folder; this folder has no profiles.
// Browser mock only: persisted local state and the existing ownership/command-plan hooks, no game or service.
import { expect, test, type Page } from "@playwright/test";
import type { GameStatus, PackInstance, Profile, ProfileSummary } from "../src/api/commands";

test.use({ permissions: ["clipboard-read", "clipboard-write"] });
test.beforeEach(async ({ context }) => {
  await context.route(/^https?:\/\//, (route) => ["localhost", "127.0.0.1"].includes(new URL(route.request().url()).hostname) ? route.continue() : route.abort());
});
const FOREIGN = { id: "my-mods", root: "/mnt/other-launcher/Kiln" };
type Mock = { calls: () => string[]; setOtherFolderProfile: (o: typeof FOREIGN | null) => void;
  planCommand: (cmd: string, plan: { gate?: string; result?: unknown; error?: { code: string; message: string } }) => void;
  openGate: (gate: string) => void; addSfGame: (path: string) => string };
const mock = <T,>(page: Page, fn: (h: Mock) => T) => page.evaluate((src) =>
  new Function("h", `return (${src})(h)`)((window as unknown as { __kilnMock: Mock }).__kilnMock), fn.toString()) as Promise<T>;
const api = <T,>(page: Page, fn: string, ...args: unknown[]) => page.evaluate(async ([f, a]) => {
  const m = await import("/src/api/commands.ts") as unknown as Record<string, (...x: unknown[]) => Promise<unknown>>;
  return m[f as string](...(a as unknown[]));
}, [fn, args] as const) as Promise<T>;
const profiles = (page: Page) => page.getByRole("region", { name: "Profiles", exact: true });
const requests = (page: Page) => page.evaluate(() => (window as unknown as {
  __kilnCrucibleMock: { requests: () => { cmd: string; args: Record<string, unknown> }[] }
}).__kilnCrucibleMock.requests());

async function foreignEmpty(page: Page, query = "") {
  await page.addInitScript(() => {
    sessionStorage.setItem("kiln.mock.persist", "1");
    sessionStorage.setItem("kiln.mock.state:1", JSON.stringify({ profiles: [], activations: {}, remembered: {}, library: [] }));
    // The real ensure-default refuses to touch another data folder's pointer. The mock's default predates ownership.
    (window as unknown as { __kilnMockPlans: unknown }).__kilnMockPlans = { profile_ensure_default: [{ result: null }] };
  });
  await page.goto(`/?mock=1${query}#/settings/mods`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await page.evaluate((owner) => (window as unknown as { __kilnMock: Mock }).__kilnMock.setOtherFolderProfile(owner), FOREIGN);
  await expect(page.getByTestId("other-folder-profile")).toBeVisible();
  expect(await api<ProfileSummary[]>(page, "profileList")).toEqual([]);
}
async function untouched(page: Page) {
  expect((await api<GameStatus>(page, "gameStatus", "client-steam")).otherFolderProfile).toEqual(FOREIGN);
  expect(await api(page, "profileActive", "client-steam")).toBeNull();
  const calls = await mock(page, (h) => h.calls());
  for (const cmd of ["profile_activate", "profile_deactivate", "launch_play", "launch_prepare", "pack_build", "crucible_share"])
    expect(calls, cmd).not.toContain(cmd);
}

test("foreign pointer and empty local folder: Got a code, preview and Install make only a local instance", async ({ page }) => {
  await foreignEmpty(page);
  await page.evaluate(() => { location.hash = "#/"; });
  await expect(page.getByTestId("crucible-got-code")).toBeVisible();
  await expect(page.getByTestId("crucible-share")).toHaveCount(0);
  await expect(page.getByTestId("crucible-save-file")).toHaveCount(0);
  await page.getByTestId("crucible-got-code").click();
  await page.getByTestId("crucible-code-input").fill("K4TW9XRB");
  await page.getByTestId("crucible-code-open").click();
  await expect(page.getByTestId("crucible-preview")).toBeVisible();
  await expect(page.getByTestId("crucible-profile-name")).toHaveValue("Cinder Trails (Cinderhawk's pack)");
  await page.getByTestId("crucible-trust").check();
  await page.getByTestId("crucible-install-only").click();
  await expect(page.getByTestId("crucible-preview")).toHaveCount(0);
  const saved = await api<ProfileSummary[]>(page, "profileList");
  expect(saved).toHaveLength(1);
  expect(saved[0].name).toBe("Cinder Trails (Cinderhawk's pack)");
  expect(await api<[string, PackInstance][]>(page, "packInstances")).toEqual([[saved[0].id, expect.objectContaining({ name: "Cinder Trails" })]]);
  const reqs = await requests(page);
  expect(reqs.filter((r) => ["crucible_share", "crucible_shared", "pack_build"].includes(r.cmd))).toEqual([]);
  expect(reqs.find((r) => r.cmd === "crucible_install")?.args).toMatchObject({ gameId: "client-steam", request: { play: false } });
  await untouched(page);
});

test("Settings creates the first empty local profile without activation or a Load it toast", async ({ page }) => {
  await foreignEmpty(page);
  await profiles(page).getByRole("button", { name: "Create empty local profile…" }).click();
  const dialog = page.getByRole("dialog", { name: "Create empty local profile", exact: true });
  await expect(dialog).toContainText("No mods or settings are copied. This only saves a profile in this launcher's data folder; it does not load it into the game.");
  await dialog.getByLabel("Name").fill("My mods");
  await dialog.getByRole("button", { name: "Save", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  const saved = await api<ProfileSummary[]>(page, "profileList");
  expect(saved).toHaveLength(1);
  const p = await api<Profile>(page, "profileGet", saved[0].id);
  expect(p.name).toBe("My mods");
  expect(p.id).toBe(FOREIGN.id); // A matching id in this folder must not adopt the foreign pointer.
  expect(Object.values(p.mods).flat()).toEqual([]);
  expect(p.dropins).toEqual([]);
  await expect(page.locator(".k-toast").filter({ hasText: "Created local profile My mods" })).toContainText("Saved locally only. The game's loaded profile has not changed.");
  expect(await page.locator(".k-toasts").getByRole("button", { name: "Load it" }).count()).toBe(0);
  await untouched(page);
});

test("Crucible off keeps receive hidden even without a local profile", async ({ page }) => {
  await foreignEmpty(page, "&crucible=off");
  await page.evaluate(() => { location.hash = "#/"; });
  await expect(page.locator(".k-hero").getByRole("link", { name: "Another data folder's profile is loaded in this game" })).toBeVisible();
  await expect(page.getByTestId("crucible-got-code")).toHaveCount(0);
  await expect(page.getByTestId("crucible-share")).toHaveCount(0);
  await untouched(page);
});

test("loading profiles cannot be mistaken for No profile", async ({ page }) => {
  await page.addInitScript(() => {
    (window as unknown as { __kilnMockPlans: unknown }).__kilnMockPlans = { profile_list: [{ gate: "profiles-ready" }] };
  });
  await page.goto("/?mock=1#/settings/mods");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(profiles(page).locator(".k-card-actions button")).toBeDisabled();
  await expect(profiles(page).getByRole("button", { name: "Create empty local profile…" })).toHaveCount(0);
  await mock(page, (h) => h.openGate("profiles-ready"));
  await expect(profiles(page).getByRole("button", { name: "Save current as…" })).toBeEnabled();
});

test("copy intent survives ownership and selection changes, never becoming empty creation", async ({ page }) => {
  await page.goto("/?mock=1#/settings/mods");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  const original = await api<Profile>(page, "profileGet", "ember-run");
  await profiles(page).getByRole("button", { name: "Save current as…" }).click();
  const dialog = page.getByRole("dialog", { name: "Save current as", exact: true });
  await dialog.getByLabel("Name").fill("My copied mods");
  await mock(page, (h) => h.setOtherFolderProfile({ id: "my-mods", root: "/mnt/other-launcher/Kiln" }));
  await expect(page.getByTestId("other-folder-profile")).toBeVisible();
  await expect(dialog).toBeVisible();
  await expect(dialog).toContainText("A copy of Ember run: its mods, switches and configs.");
  await page.evaluate(async () => {
    const id = (window as unknown as { __kilnMock: Mock }).__kilnMock.addSfGame("/mnt/second-game");
    const { useGameSel } = await import("/src/api/hooks.ts");
    useGameSel.getState().setGameId(id);
  });
  await dialog.getByRole("button", { name: "Save", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  const p = await api<Profile>(page, "profileGet", "my-copied-mods");
  expect(p.mods).toEqual(original.mods);
  expect(p.dropins).toEqual(original.dropins);
  expect((await api<GameStatus>(page, "gameStatus", "client-steam")).otherFolderProfile).toEqual(FOREIGN);
  await expect(page.locator(".k-toast").filter({ hasText: "Saved as My copied mods" })).toBeVisible();
  expect(await page.locator(".k-toasts").getByRole("button", { name: "Load it" }).count()).toBe(0);
  expect(await mock(page, (h) => h.calls())).not.toContain("profile_activate");
});

test("copy failure stays a copy error, with no blank fallback", async ({ page }) => {
  await page.goto("/?mock=1#/settings/mods");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await profiles(page).getByRole("button", { name: "Save current as…" }).click();
  const dialog = page.getByRole("dialog", { name: "Save current as", exact: true });
  await dialog.getByLabel("Name").fill("Failed copy");
  await mock(page, (h) => {
    h.planCommand("profile_create", { error: { code: "not_found", message: "No such profile to copy." } });
    h.setOtherFolderProfile({ id: "my-mods", root: "/mnt/other-launcher/Kiln" });
  });
  await expect(page.getByTestId("other-folder-profile")).toBeVisible();
  await dialog.getByRole("button", { name: "Save", exact: true }).click();
  await expect(page.locator(".k-toast").filter({ hasText: "No such profile to copy." })).toBeVisible();
  await expect(dialog).toBeVisible();
  expect((await api<ProfileSummary[]>(page, "profileList")).some((p) => p.name === "Failed copy")).toBe(false);
  expect((await mock(page, (h) => h.calls())).filter((c) => c === "profile_create")).toHaveLength(1);
});

for (const cmd of ["profile_list", "profile_active"]) {
  test(`${cmd} errors cannot offer blank creation instead of a copy`, async ({ page }) => {
    await page.addInitScript((command) => {
      const error = { code: "io", message: "Profile data is unreadable." };
      (window as unknown as { __kilnMockPlans: unknown }).__kilnMockPlans = { [command]: Array.from({ length: 8 }, () => ({ error })) };
    }, cmd);
    await page.goto("/?mock=1#/settings/mods");
    await page.locator(".k-splash").waitFor({ state: "detached" });
    await expect.poll(async () => (await mock(page, (h) => h.calls())).filter((c) => c === cmd).length).toBeGreaterThanOrEqual(4);
    await expect(profiles(page).locator(".k-card-actions button")).toBeDisabled();
    await expect(profiles(page).getByRole("button", { name: "Create empty local profile…" })).toHaveCount(0);
    expect(await mock(page, (h) => h.calls())).not.toContain("profile_create");
  });
}

test("a selected game can receive a code before Kiln or a local profile exists", async ({ page }) => {
  await page.goto("/?mock=fresh#/");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await expect(page.locator(".k-hero")).toContainText("Kiln is not in this game yet.");
  await expect(page.getByTestId("crucible-got-code")).toBeVisible();
  await expect(page.getByTestId("crucible-share")).toHaveCount(0);
  await expect(page.getByTestId("crucible-save-file")).toHaveCount(0);
  expect((await requests(page)).filter((r) => ["crucible_shared", "crucible_share", "pack_build"].includes(r.cmd))).toEqual([]);
});
