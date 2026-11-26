// "Open web panel" with the renamed web panel mod (0.2.0-dev.31.14): Kiln Web Admin is now Kilnwarden (mod id
// `kilnwarden`). The button shows for either id; with both enabled Kilnwarden stays off by itself (it logs why) and the
// old copy's panel opens, with nothing extra on the Overview. The login is minted for the mod that listens.
// Browser mock: src/pages/servers/mockServers.ts (`__kilnMock.webPanelMods`).
//   bun x playwright test tests/web-panel-kilnwarden.spec.ts
import { expect, test, type Page } from "@playwright/test";

type PanelMod = { id: string; enabled: boolean };
type Opened = { serverId: string; mod: string; consumer: string };
type Hooks = { webPanelMods: { set: (l: PanelMod[]) => void; opens: () => Opened[] }; setServer: (o: { running?: boolean }) => void; planCommand: (cmd: string, plan: unknown) => void };
declare global { interface Window { __kilnMock: Hooks } }

async function open(page: Page, route = "/") {
  await page.goto(`/#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
  if (await page.evaluate((r) => location.hash !== `#${r}`, route)) await page.evaluate((r) => { location.hash = `#${r}`; }, route);
}

/** The lab server's Overview with `mods` in its mods folder, running unless `running` is false. */
async function overview(page: Page, mods: PanelMod[], running = true) {
  await open(page, "/");
  await page.evaluate((a) => { window.__kilnMock.webPanelMods.set(a.mods); window.__kilnMock.setServer({ running: a.running }); }, { mods, running });
  await page.evaluate(() => { location.hash = "#/servers/lab/overview"; });
  await expect(page.locator(".k-sstats")).toBeVisible();
}
const button = (page: Page) => page.getByRole("button", { name: "Open web panel" });
const opens = (page: Page) => page.evaluate(() => window.__kilnMock.webPanelMods.opens());

test("Kilnwarden on the server: Open web panel shows and opens its panel, the login minted for kilnwarden", async ({ page }) => {
  await overview(page, [{ id: "kilnwarden", enabled: true }]);
  await expect(button(page)).toBeVisible();
  await button(page).click();
  await expect.poll(() => opens(page)).toEqual([{ serverId: "lab", mod: "kilnwarden", consumer: "kilnwarden" }]);
  await expect(page.getByText("Could not open the web panel")).toHaveCount(0);
});

test("a Kiln Web Admin 0.1.0 test build still opens, the login minted for kiln-webadmin", async ({ page }) => {
  await overview(page, [{ id: "kiln-webadmin", enabled: true }]);
  await button(page).click();
  await expect.poll(() => opens(page)).toEqual([{ serverId: "lab", mod: "kiln-webadmin", consumer: "kiln-webadmin" }]);
  await expect(page.getByText("Could not open the web panel")).toHaveCount(0);
});

test("both installed: one button, no warning of the launcher's own; the old copy's panel is the one listening", async ({ page }) => {
  await overview(page, [{ id: "kiln-webadmin", enabled: true }, { id: "kilnwarden", enabled: true }]);
  await expect(button(page)).toHaveCount(1);
  await button(page).click();
  await expect.poll(() => opens(page)).toEqual([{ serverId: "lab", mod: "kiln-webadmin", consumer: "kiln-webadmin" }]);
  await expect(page.getByText("Could not open the web panel")).toHaveCount(0);
});

test("Kilnwarden switched off and nothing else: no button", async ({ page }) => {
  await overview(page, [{ id: "kilnwarden", enabled: false }]);
  await expect(button(page)).toHaveCount(0);
});

test("Kilnwarden switched off, the old copy on: the old copy's panel", async ({ page }) => {
  await overview(page, [{ id: "kilnwarden", enabled: false }, { id: "kiln-webadmin", enabled: true }]);
  await button(page).click();
  await expect.poll(() => opens(page)).toEqual([{ serverId: "lab", mod: "kiln-webadmin", consumer: "kiln-webadmin" }]);
});

test("a stopped server has no web panel button", async ({ page }) => {
  await overview(page, [{ id: "kilnwarden", enabled: true }], false);
  await expect(button(page)).toHaveCount(0);
});

test("Kilnwarden not listening yet: the Overview says so", async ({ page }) => {
  await overview(page, [{ id: "kilnwarden", enabled: true }]);
  await page.evaluate(() => window.__kilnMock.planCommand("admin_open_web_panel", { error: { code: "admin_unavailable", message: "Kilnwarden is not listening. Check that the current mod is loaded." } }));
  await button(page).click();
  await expect(page.getByText("Could not open the web panel")).toBeVisible();
  await expect(page.getByText("Kilnwarden is not listening.")).toBeVisible();
  expect(await opens(page)).toEqual([]);
});
