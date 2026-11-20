import { expect, test } from "@playwright/test";

const option = 'WINEDLLOVERRIDES="dinput8,xinput9_1_0=n,b" %command%';

test("desktop advises for the chained loader when Kiln's override is already set", async ({ page }) => {
  await page.goto("/?mock=chained-override#/");
  await expect(page.getByText("One Steam setting for Ultimate ASI Loader", { exact: true })).toBeVisible();
  await expect(page.getByText(option, { exact: true })).toBeVisible();
  await expect(page.getByText("Kiln is not loading", { exact: true })).toHaveCount(0);
});

test("Deck Home and Help report the missing chained override", async ({ page }) => {
  await page.goto("/?mock=chained-override&deck=steamdeck#/");
  await expect(page.getByText("One Steam setting for Ultimate ASI Loader", { exact: true })).toBeVisible();
  await page.getByText("One Steam setting for Ultimate ASI Loader", { exact: true }).click();
  await expect(page.getByText("Add xinput9_1_0", { exact: true })).toBeVisible();
  await expect(page.getByTestId("deck-launch-option")).toHaveText(option);
  await expect(page.getByText("Already set", { exact: true })).toHaveCount(0);
});

test("ordinary Deck install still reports Already set", async ({ page }) => {
  await page.goto("/?deck=steamdeck#/deck/help");
  await expect(page.getByText("Already set", { exact: true })).toBeVisible();
  await expect(page.getByText("Add xinput9_1_0", { exact: true })).toHaveCount(0);
});
