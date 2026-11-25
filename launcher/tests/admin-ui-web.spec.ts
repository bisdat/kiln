// @kiln/admin-ui outside the launcher: the admin pages in a plain web page (src/admin/harness, the package's entry from
// source) over the WebSocket transport, with a fake KAP/1-over-WebSocket server behind it (Playwright's routeWebSocket)
// that runs the same in-memory Kiln.Admin as the launcher's browser mock (src/admin/mock/world.ts, the committed
// synthetic fixture). The page has a Tauri IPC stand-in, as if opened inside the launcher's webview: nothing may call
// it, and none of the launcher's command layer, its mock or Tauri is even loaded.
import { expect, test, type Page } from "@playwright/test";
import { kapServer } from "./fixtures/kapServer";

const HARNESS = "/src/admin/harness/index.html?kap=ws://kap.test/kap";
const LAUNCHER_ONLY = /\/src\/api\/(commands|adminTauri|mock|fallback)|\/src\/admin\/mock\/|tauri-apps|\/src\/components\//;
const LOAD = { timeout: 30_000 };

/** Tauri's IPC as the launcher's webview has it, counting calls; and every URL the page loads. */
async function watch(page: Page) {
  await page.addInitScript(() => {
    const w = window as unknown as { __TAURI_INTERNALS__: unknown; __tauriCalls: string[] };
    w.__tauriCalls = [];
    w.__TAURI_INTERNALS__ = { invoke: (cmd: string) => { w.__tauriCalls.push(cmd); return Promise.resolve(null); }, transformCallback: () => 0, convertFileSrc: (p: string) => p };
  });
  const loaded: string[] = [];
  page.on("request", (r) => loaded.push(r.url()));
  return {
    loaded,
    tauriCalls: () => page.evaluate(() => (window as unknown as { __tauriCalls: string[] }).__tauriCalls),
  };
}

test("the admin pages run in a plain page over KAP/1 on a WebSocket, without the launcher", async ({ page }) => {
  const w = await watch(page);
  const kap = await kapServer(page);
  await page.goto(`${HARNESS}#/servers/lab/admin/players`);
  // the live player list from the state pushes
  await expect(page.getByText("Brannoc").first()).toBeVisible(LOAD);
  await expect(page.getByText("Ysolde").first()).toBeVisible();
  expect(kap.urls).toEqual(["ws://kap.test/kap/lab"]);
  expect(kap.frames.find((f) => f.t === "sub")).toEqual({ t: "sub", id: expect.any(Number), topic: "state" });
  expect(kap.frames.some((f) => f.t === "auth")).toBe(false);

  // an action is a KAP request on the same socket
  await page.evaluate(() => { location.hash = "#/servers/lab/admin/world"; });
  const text = page.getByRole("textbox", { name: "Broadcast text" });
  await expect(text).toBeVisible(LOAD);
  await text.fill("The server restarts soon.");
  await text.press("Enter");
  await expect(page.getByText(/Sent to \d+ players?/)).toBeVisible();
  expect(kap.frames.filter((f) => f.t === "req" && f.op === "broadcast")).toEqual([{ t: "req", id: expect.any(Number), op: "broadcast", args: { text: "The server restarts soon." } }]);
  expect(kap.urls).toHaveLength(1);  // one session for every page

  expect(w.loaded.filter((u) => LAUNCHER_ONLY.test(u))).toEqual([]);
  expect(await w.tauriCalls()).toEqual([]);
});

test("without the launcher: no transport choice when offline, no launcher restart for a staged restore", async ({ page }) => {
  const w = await watch(page);
  await kapServer(page, (world) => { world.run("backup_restore", { op: "backup_restore", name: "nightly" }); });
  await page.goto(`${HARNESS}#/servers/offline/admin/players`);
  await expect(page.getByText("Not connected")).toBeVisible(LOAD);
  await expect(page.getByRole("button", { name: "Reconnect" })).toBeVisible();
  await expect(page.getByRole("combobox", { name: "Transport" })).toHaveCount(0);

  await page.evaluate(() => { location.hash = "#/servers/lab/admin/saves"; });
  await expect(page.getByText(/A restore is staged/)).toBeVisible(LOAD);
  await expect(page.getByRole("button", { name: "Cancel restore" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Restart now" })).toHaveCount(0);
  expect(w.loaded.filter((u) => LAUNCHER_ONLY.test(u))).toEqual([]);
  expect(await w.tauriCalls()).toEqual([]);
});

test("the launcher keeps its transport choice and its restart for a staged restore", async ({ page }) => {
  // the same pages in the launcher (browser mock): the Tauri transport's four kinds and the launcher's own restart
  await page.goto("/#/servers/offline/admin/players");
  await expect(page.getByRole("combobox", { name: "Transport" })).toBeVisible(LOAD);
  await page.goto("/#/servers/lab/admin/saves");
  await page.getByRole("button", { name: "Restore at next start…" }).first().click(LOAD);
  const dialog = page.getByRole("dialog");
  await dialog.locator("#k-confirm-word").fill("restore");
  await dialog.getByRole("button", { name: "Stage the restore" }).click();
  await expect(page.getByText(/A restore is staged/)).toBeVisible(LOAD);
  await expect(page.getByRole("button", { name: "Restart now" })).toBeVisible();
});
