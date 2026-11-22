// A server's Mods page names a Kiln mod that opts into Steam peer messages ("steamMessages": true in kiln.mod.json,
// since 0.2.0-dev.30.3) in its facts: "Steam: Talks to other players over Steam" (server_mods' steamMessages). A mod
// without it has no such line. Browser mock (src/pages/servers/mockServers.ts: Isle Icons opts in, Kiln.Admin does not).
//   bun x playwright test tests/server-mods-steam.spec.ts
import { expect, test, type Page } from "@playwright/test";
import { mkdirSync } from "node:fs";

const SHOTS = process.env.SHOTS_DIR;

async function open(page: Page, route = "/") {
  await page.goto(`/#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
  if (await page.evaluate((r) => location.hash !== `#${r}`, route)) await page.evaluate((r) => { location.hash = `#${r}`; }, route);
}

test("a server mod that talks to other players over Steam says so in its facts, another mod does not", async ({ page }) => {
  await open(page, "/servers/lab/mods");
  const aside = page.getByRole("complementary", { name: "Selected mod" });
  await page.getByRole("table", { name: "Server mods" }).getByText("isle-icons", { exact: true }).first().click();
  await expect(aside.getByText("isle-icons", { exact: false }).first()).toBeVisible();
  await expect(aside.getByText("Talks to other players over Steam", { exact: true })).toBeVisible();
  await expect(aside.getByText("Steam", { exact: true })).toBeVisible();
  if (SHOTS) { mkdirSync(SHOTS, { recursive: true }); await aside.screenshot({ path: `${SHOTS}/server-mod-steam.png` }); }
  await page.getByRole("table", { name: "Server mods" }).getByText("kiln-admin", { exact: true }).first().click();
  await expect(aside.getByText("Serves the live pages", { exact: true })).toBeVisible();
  await expect(aside.getByText("Talks to other players over Steam", { exact: true })).toHaveCount(0);
});
