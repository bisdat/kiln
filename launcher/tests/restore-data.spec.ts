import { expect, test } from "@playwright/test";
test("restore data lists files and requires Steam confirmation before moving", async ({ page }) => {
  await page.goto("/?restoreData=1#/logs");
  await page.locator(".k-splash").waitFor({ state: "detached" });
  await page.getByRole("tab", { name: "Engines" }).click();
  await page.getByRole("button", { name: "Restore the game's data", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "Restore the game's data" });
  await expect(dialog).toContainText("enshrouded_055.dat");
  const move = dialog.getByRole("button", { name: "Move files aside" });
  await expect(move).toBeDisabled();
  await dialog.getByRole("checkbox").check();
  await move.click();
  await expect(dialog).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Undo move" })).toBeVisible();
  await page.getByRole("button", { name: "Undo move" }).click();
  await expect(page.getByRole("button", { name: "Undo move" })).toHaveCount(0);
});
