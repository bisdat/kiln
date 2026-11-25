// "Open web panel" on a server's Overview (0.2.0-dev.31.12). Each server's panel opens through the launcher's gateway
// on the server's own loopback origin (cookies of one panel never reach another), the browser is sent a one-use
// address instead of the one-time secret, and the gateway closes by itself after the panel's last tab, or on Close.
// Browser mock: src/pages/servers/mockServers.ts (`__kilnMock.webPanel`).
//   bun x playwright test tests/web-panel.spec.ts
import { expect, test, type Page } from "@playwright/test";

async function open(page: Page, route = "/") {
  await page.goto(`/#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
  if (await page.evaluate((r) => location.hash !== `#${r}`, route)) await page.evaluate((r) => { location.hash = `#${r}`; }, route);
}
/** Runs `body` with `h` = the mock's test hooks (`__kilnMock`). */
const withHooks = (page: Page, body: string) => page.evaluate(`(() => { const h = window.__kilnMock; ${body} })()`);

/** The lab server's Overview with Kiln Web Admin loaded and the server running. */
async function overview(page: Page, via = "local") {
  await open(page, "/");
  await withHooks(page, `h.webPanel.install(true); h.webPanel.setVia(${JSON.stringify(via)}); h.setServer({ running: true });`);
  await page.evaluate(() => { location.hash = "#/servers/lab/overview"; });
  await expect(page.getByRole("button", { name: "Open web panel" })).toBeVisible();
}

test("Open web panel shows the server's own origin, its certificate and when it closes; Close closes it", async ({ page }) => {
  await overview(page);
  await expect(page.getByRole("button", { name: "Close web panel" })).toHaveCount(0);
  await page.getByRole("button", { name: "Open web panel" }).click();
  const status = page.getByRole("list", { name: "Open web panel" });
  await expect(status.getByRole("listitem").first()).toHaveText(/^Web panel open at https:\/\/127\.\d+\.\d+\.\d+:\d+$/);
  await expect(status).toContainText("Certificate AB:AB:AB:AB…AB:AB");
  await expect(status).toContainText("It closes by itself 2 minutes after its last tab.");
  await expect(status).not.toContainText("share their sign-in cookies");
  // the origin is the server's own, not the shared 127.0.0.1
  const origin = (await status.getByRole("listitem").first().textContent())!.replace("Web panel open at ", "");
  expect(origin.startsWith("https://127.0.0.1")).toBe(false);
  const listed = await page.evaluate(() => (window as unknown as { __kilnMock: { webPanel: { list: () => { origin: string }[] } } }).__kilnMock.webPanel.list());
  expect(listed.map((p) => p.origin)).toEqual([origin]);
  // Close: the gateway goes, the status line too
  await page.getByRole("button", { name: "Close web panel" }).click();
  await expect(status).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Close web panel" })).toHaveCount(0);
  const calls = await page.evaluate(() => (window as unknown as { __kilnMock: { calls: () => string[] } }).__kilnMock.calls());
  expect(calls).toContain("admin_open_web_panel");
  expect(calls).toContain("admin_close_web_panel");
  expect(calls).toContain("admin_web_panels");
});

test("a gateway that closed by itself is not shown any more, and opening again works", async ({ page }) => {
  await overview(page);
  await page.getByRole("button", { name: "Open web panel" }).click();
  await expect(page.getByRole("list", { name: "Open web panel" })).toBeVisible();
  // the panel's last tab closed: the gateway went by itself
  await withHooks(page, `h.webPanel.expire("lab");`);
  await page.evaluate(() => { location.hash = "#/servers/lab/console"; });
  await page.evaluate(() => { location.hash = "#/servers/lab/overview"; });
  await expect(page.getByRole("button", { name: "Open web panel" })).toBeVisible();
  await expect(page.getByRole("list", { name: "Open web panel" })).toHaveCount(0);
  await page.getByRole("button", { name: "Open web panel" }).click();
  await expect(page.getByRole("list", { name: "Open web panel" })).toBeVisible();
});

test("through SSH, the status says so", async ({ page }) => {
  await overview(page, "ssh");
  await page.getByRole("button", { name: "Open web panel" }).click();
  await expect(page.getByRole("list", { name: "Open web panel" }).getByRole("listitem").first()).toHaveText(/ \(through SSH\)$/);
});

test("a panel that can't be opened says why", async ({ page }) => {
  await overview(page);
  await withHooks(page, `h.planCommand("admin_open_web_panel", { error: { code: "unsupported", message: "Kiln Web Admin listens on 192.0.2.10:15737, which is another machine." } });`);
  await page.getByRole("button", { name: "Open web panel" }).click();
  const banner = page.locator(".k-sov-webpanel").getByText("Could not open the web panel");
  await expect(banner).toBeVisible();
  await expect(page.locator(".k-sov-webpanel")).toContainText("which is another machine");
  await expect(page.getByRole("list", { name: "Open web panel" })).toHaveCount(0);
});

test("without Kiln Web Admin on the server there is no web panel button", async ({ page }) => {
  await open(page, "/");
  await withHooks(page, `h.webPanel.install(false); h.setServer({ running: true });`);
  await page.evaluate(() => { location.hash = "#/servers/lab/overview"; });
  await expect(page.locator(".k-sstats")).toBeVisible();
  await expect(page.getByRole("button", { name: "Open web panel" })).toHaveCount(0);
});
