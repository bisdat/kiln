import { expect, test } from '@playwright/test';

test('native profile row offers Replace DLL; engine rows do not', async ({ page }) => {
  await page.goto('/?kept=1#/');
  await page.locator('.k-splash').waitFor({ state: 'detached' });
  const rows = page.locator('.k-hero').getByRole('list', { name: 'Mods' }).getByRole('listitem');
  const native = rows.filter({ has: page.getByText('FPS Unlock', { exact: true }) }).first();
  await native.getByRole('button', { name: /More for/ }).click();
  await expect(page.getByRole('menuitem', { name: 'Replace DLL…' })).toBeVisible();
  // The browser mock has no native file picker; the desktop app enables this item.
  await expect(page.getByRole('menuitem', { name: 'Replace DLL…' })).toBeDisabled();
  await page.keyboard.press('Escape');
  const engine = rows.filter({ has: page.getByText('Zoom Out', { exact: true }), hasNot: page.locator('.k-mod-where') });
  await engine.getByRole('button', { name: /More for/ }).click();
  await expect(page.getByRole('menuitem', { name: 'Replace DLL…' })).toHaveCount(0);
});
