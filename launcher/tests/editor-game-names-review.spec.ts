// Invented localization only; no installed-game text or player saves.
import { expect, test, type Locator } from '@playwright/test';

const locales = ['en', 'de', 'es', 'fr', 'it', 'ja', 'ko', 'pl', 'pt-BR', 'ru', 'zh-CN'];
const sizes = [{ width: 1024, height: 640 }, { width: 1280, height: 720 }, { width: 1920, height: 1080 }];
const label = '测试名称 テスト 이름 اسم تجريبي FixtureUnbrokenLocalizedNameForContainment';
const languages = Object.fromEntries(['en-us', 'de-de', 'es-es', 'fr-fr', 'it-it', 'ja-jp', 'ko-kr', 'pl-pl', 'pt-br', 'ru-ru', 'zh-cn'].map((code) => [code, {
  items: { 61577244: label }, skills: { 29426941: label }, knowledge: { 40769: label },
}]));
async function fits(child: Locator, parent: Locator) {
  const a = (await child.boundingBox())!, b = (await parent.boundingBox())!;
  expect(a).not.toBeNull(); expect(b).not.toBeNull();
  expect(a.x).toBeGreaterThanOrEqual(b.x - 1);
  expect(a.x + a.width).toBeLessThanOrEqual(b.x + b.width + 1);
  expect(await child.evaluate((el) => el.scrollWidth <= el.clientWidth + 1)).toBe(true);
}

for (const locale of locales) for (const viewport of [...sizes, { width: 1280, height: 800 }]) {
  const deck = viewport.height === 800;
  test(`localized CJK and RTL names fit ${locale} ${viewport.width}x${viewport.height} ${deck ? 'Deck' : 'desktop'}`, async ({ browser }) => {
    const context = await browser.newContext({ locale, viewport });
    const page = await context.newPage();
    await page.addInitScript((locale) => Object.defineProperty(navigator, 'languages', { get: () => [locale] }), locale);
    await page.route('**/assets-local/editor/names/names.json*', (route) => route.fulfill({ json: { format: 1, languages } }));
    await page.goto(deck ? '/?deck=steamdeck#/deck/editor' : '/#/editor');
    const slot = page.locator('[data-deck-id="slot-main:0"]');
    await expect(slot).toHaveAccessibleName(new RegExp('测试名称'), { timeout: 15000 });
    await slot.click();
    await fits(page.locator('.ke-insp-name'), page.locator('.ke-insp'));
    if (deck) await page.locator('[data-deck-id="slot-main:10"]').click();
    else await page.locator('.ke-group-head button').first().click();
    const catalog = page.locator('.ke-cat');
    await catalog.locator('input').first().fill('FixtureUnbroken');
    await catalog.getByRole('option', { name: label, exact: true }).click();
    await fits(catalog.locator('.ke-insp-name'), catalog.locator('.ke-cat-pick'));
    if (deck) await page.evaluate(() => {
      for (const pressed of [true, false]) (window as any).__kilnMock.emit('kiln://gamepad', { kind: 'button', button: 'b', pressed, repeat: false, device: 'event9' });
    });
    else await page.keyboard.press('Escape');
    await expect(catalog).toHaveCount(0);
    await (deck ? page.locator('[data-deck-id="ed-tab-skills"]') : page.getByRole('tab').nth(2)).click();
    const node = page.locator(deck ? '[data-deck-id="node-29426941"]' : '.ke-node[aria-label*="测试名称"]');
    await node.focus();
    await expect(page.locator('.ke-tree-tip-name')).toHaveText(label);
    await fits(page.locator('.ke-tree-tip-name'), page.locator('.ke-tree-tip'));
    await (deck ? page.locator('[data-deck-id="ed-tab-know"]') : page.getByRole('tab').nth(3)).click();
    await page.locator('[data-deck-id="know-search"]').fill('测试名称');
    await expect(page.locator('.ke-know-body [role="row"]')).toHaveCount(1);
    await fits(page.locator('.ke-know-body .ke-know-name'), page.locator('.ke-know-table'));
    await context.close();
  });
}

test('Knowledge search folds Latin accents and full-width text', async ({ page }) => {
  await page.route('**/assets-local/editor/names/names.json*', (route) => route.fulfill({ json: { format: 1, languages: {
    'en-us': { knowledge: { 40769: 'Fixture Café ＡＢＣ' } },
  } } }));
  await page.goto('/#/editor');
  await expect(page.locator('.ke .k-page-title')).toContainText('Aldric', { timeout: 15000 });
  await page.getByRole('tab').nth(3).click();
  for (const query of ['cafe', 'CAFÉ', 'abc', 'ＡＢＣ']) {
    await page.locator('[data-deck-id="know-search"]').fill(query);
    await expect(page.locator('.ke-know-body [role="row"]')).toHaveCount(1);
  }
});

test('a large multilingual names table loads and catalog typing stays responsive', async ({ page }) => {
  const rows = Object.fromEntries(Array.from({ length: 12000 }, (_, id) => [id, `Fixture localized name ${id} 测试名称` ]));
  const big = { format: 1, languages: Object.fromEntries(Array.from({ length: 15 }, (_, i) => [i === 0 ? 'en-us' : `fixture-${i}`, {
    items: { ...rows, 61577244: 'Fixture searchable item' }, skills: {}, knowledge: {},
  }])) };
  expect(Buffer.byteLength(JSON.stringify(big), 'utf8')).toBeGreaterThan(8_000_000);
  await page.route('**/assets-local/editor/names/names.json*', (route) => route.fulfill({ json: big }));
  const start = Date.now();
  await page.goto('/#/editor');
  await expect(page.locator('[data-deck-id="slot-main:0"]')).toHaveAccessibleName(/Fixture searchable item/, { timeout: 15000 });
  expect(Date.now() - start).toBeLessThan(15000);
  await page.locator('.ke-group-head button').first().click();
  const search = page.locator('.ke-cat input').first();
  await search.fill('Fixture searchable');
  await expect(page.locator('.ke-cat [role="option"]')).toHaveCount(1);
  const timing = await page.evaluate(async () => {
    const path = '/src/editor/kiln/Catalog.jsx';
    const { allItems, filterItems } = await import(/* @vite-ignore */ path);
    const all = allItems();
    const start = performance.now();
    for (let i = 0; i < 100; i++) filterItems(all, { q: `Fixture searchable ${i}` });
    return performance.now() - start;
  });
  expect(timing).toBeLessThan(1000);
});
