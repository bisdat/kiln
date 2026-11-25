// Copying remains local even if the same game starts using another launcher's data folder while the modal is open.
import { expect, test } from "@playwright/test";

test.beforeEach(async ({ context }) => {
  await context.route(/^https?:\/\//, (route) => ["localhost", "127.0.0.1"].includes(new URL(route.request().url()).hostname) ? route.continue() : route.abort());
});

for (const foreign of [false, true]) {
  test(`copy toast ${foreign ? "omits Load it after same-game foreign ownership change" : "keeps Load it for a normal local copy"}`, async ({ page }) => {
    await page.goto("/?mock=1#/settings/mods");
    await page.locator(".k-splash").waitFor({ state: "detached" });
    const original = await page.evaluate(async () => (await import("/src/api/commands.ts")).profileGet("ember-run"));
    const profiles = page.getByRole("region", { name: "Profiles", exact: true });
    await profiles.getByRole("button", { name: "Save current as…" }).click();
    const dialog = page.getByRole("dialog", { name: "Save current as", exact: true });
    await dialog.getByLabel("Name").fill("Toast copy");
    if (foreign) {
      await page.evaluate(() => (window as unknown as { __kilnMock: {
        setOtherFolderProfile: (o: { id: string; root: string }) => void;
      } }).__kilnMock.setOtherFolderProfile({ id: "my-mods", root: "/mnt/other-launcher/Kiln" }));
      await expect(page.getByTestId("other-folder-profile")).toBeVisible();
    }
    await dialog.getByRole("button", { name: "Save", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    const toast = page.locator(".k-toast").filter({ hasText: "Saved as Toast copy" });
    await expect(toast).toBeVisible();
    // Check while the toast is present, not after its auto-dismiss could make an absence assertion pass.
    expect(await toast.getByRole("button", { name: "Load it" }).count()).toBe(foreign ? 0 : 1);
    // The existing, explicit Settings Load stays available. Saving itself never invokes it.
    await expect(profiles.getByRole("listitem").filter({ hasText: "Toast copy" }).getByRole("button", { name: "Load", exact: true })).toBeEnabled();
    const saved = await page.evaluate(async () => (await import("/src/api/commands.ts")).profileGet("toast-copy"));
    expect(saved.mods).toEqual(original.mods);
    const state = await page.evaluate(async () => {
      const api = await import("/src/api/commands.ts");
      const calls = (window as unknown as { __kilnMock: { calls: () => string[] } }).__kilnMock.calls();
      return { status: await api.gameStatus("client-steam"), calls };
    });
    expect(state.status.otherFolderProfile ?? null).toEqual(foreign ? { id: "my-mods", root: "/mnt/other-launcher/Kiln" } : null);
    expect(state.calls).not.toContain("profile_activate");
    expect(state.calls).not.toContain("profile_deactivate");
  });
}
