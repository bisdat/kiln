import { expect, test, type Page } from '@playwright/test';

async function pad(page: Page, button: string) {
  for (const pressed of [true, false]) await page.evaluate(([button, pressed]) => (window as any).__kilnMock.emit('kiln://gamepad',
    { kind: 'button', button, pressed, repeat: false, device: 'event9' }), [button, pressed] as const);
}
const focused = (page: Page) => page.evaluate(() => (document.activeElement as HTMLElement)?.dataset.deckId);

test('storage is read-only; return backs up, writes LOCAL pages, then cannot repeat', async ({ page }) => {
  await page.goto('/?storage=sample#/editor');
  await expect(page.locator('.k-page-title')).toContainText('Aldric');
  await page.getByRole('tab', { name: 'Storage', exact: true }).focus();
  await page.keyboard.press('Enter');
  const storage = page.getByRole('region', { name: 'Player storage' });
  await expect(storage).toContainText('Read-only');
  await expect(storage).toContainText('sample:bag');
  await expect(storage.locator('input')).toHaveCount(0);
  await page.getByRole('button', { name: 'Return to character' }).click();
  const ask = page.getByRole('dialog', { name: 'Return storage to this character?' });
  await expect(ask).toContainText('backup');
  await ask.getByRole('button', { name: 'Return items', exact: true }).click();
  await expect(storage).toContainText('Returned 3 stacks');
  await expect(storage).toContainText('0 stacks remain');
  await page.getByRole('button', { name: 'Return to character' }).click();
  await ask.getByRole('button', { name: 'Return items', exact: true }).click();
  await expect(storage).toContainText('Returned 0 stacks');
});

test('restore warning names storage and server history only when present', async ({ page }) => {
  await page.goto('/?storage=server#/editor');
  await expect(page.locator('.k-page-title')).toContainText('Aldric');
  await page.getByRole('button', { name: 'Backups', exact: true }).click();
  await expect(page.getByRole('dialog', { name: 'Backups for Aldric' })).toContainText('This character has player storage');
  await page.getByRole('button', { name: 'Close', exact: true }).click();
  await page.getByRole('tab', { name: 'Storage', exact: true }).click();
  await page.getByRole('button', { name: 'Return to character' }).click();
  await expect(page.getByRole('dialog', { name: 'Return storage to this character?' })).toContainText('server keeps its own copy');
});

test('running game and Steam characters cannot return storage', async ({ page }) => {
  await page.goto('/?storage=steam#/editor');
  await expect(page.locator('.k-page-title')).toContainText('Aldric');
  await page.getByRole('tab', { name: 'Storage', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Return to character' })).toBeDisabled();
  await expect(page.getByRole('region', { name: 'Player storage' })).toContainText('LOCAL');
});

test('Deck storage and return confirmation use controller focus targets', async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto('/?deck=steamdeck&storage=sample#/deck/editor');
  await expect(page.locator('.dke-name')).toContainText('Aldric');
  await expect.poll(() => focused(page)).toBe('slot-main:0');
  for (const part of ['skills', 'know', 'storage']) {
    await pad(page, 'rt');
    await expect(page.locator(`[data-deck-id="ed-tab-${part}"]`)).toHaveClass(/on/);
  }
  await expect.poll(() => focused(page)).toBe('storage-return');
  await pad(page, 'a');
  const dialog = page.getByRole('dialog', { name: 'Return storage to this character?' });
  await expect(dialog).toBeVisible();
  await expect.poll(() => focused(page)).toBe('storage-cancel');
  await pad(page, 'right');
  await expect.poll(() => focused(page)).toBe('storage-confirm');
  await pad(page, 'a');
  await expect(page.getByRole('region', { name: 'Player storage' })).toContainText('Returned 3 stacks');
});

test('running game disables return without touching either file', async ({ page }) => {
  await page.goto('/?storage=sample#/editor');
  await expect(page.locator('.k-page-title')).toContainText('Aldric');
  await page.getByRole('tab', { name: 'Storage', exact: true }).click();
  await page.evaluate(() => (window as any).__kilnMock.startOutside(5));
  await expect(page.getByRole('button', { name: 'Return to character' })).toBeDisabled();
  await expect(page.getByRole('region', { name: 'Player storage' })).toContainText('Close Enshrouded');
});
