import { expect, test } from '@playwright/test';
import { readFileSync } from 'node:fs';
const zh = JSON.parse(readFileSync(new URL('../src/i18n/zh-CN.json', import.meta.url), 'utf8'));

// Invented fixture strings, not redistributed game localization.
const names = { format: 1, languages: {
  'en-us': { items: { 61577244: 'Fixture Diamond' }, skills: { 29426941: 'Fixture Hook' }, knowledge: { 40769: 'Fixture Recipe' } },
  'zh-cn': { items: { 61577244: '测试钻石' }, skills: { 29426941: '测试钩爪' }, knowledge: { 40769: '测试配方' } },
} };

for (const deck of [false, true]) {
  test(`${deck ? 'Deck' : 'desktop'}: extracted Chinese names in inventory, Add item, skills and Knowledge`, async ({ browser }) => {
    const context = await browser.newContext({ locale: 'zh-CN', viewport: deck ? { width: 1280, height: 800 } : { width: 1360, height: 860 } });
    const page = await context.newPage();
    await page.addInitScript(() => Object.defineProperty(navigator, 'languages', { get: () => ['zh-CN'] }));
    await page.route('**/assets-local/editor/names/names.json*', (route) => route.fulfill({ json: names }));
    await page.goto(deck ? '/?deck=steamdeck#/deck/editor' : '/#/editor');
    await expect(page.locator(deck ? '.dke-name' : '.ke .k-page-title')).toContainText('Aldric', { timeout: 15000 });
    const slot = page.locator('[data-deck-id="slot-main:0"]');
    await expect(slot).toHaveAttribute('aria-label', /测试钻石/);
    await slot.click();
    await expect(page.locator('.ke-insp')).toContainText('测试钻石');
    if (deck) await page.locator('[data-deck-id="slot-main:10"]').click();
    else await page.locator('.ke-group-head button').first().click();
    const catalog = page.locator('.ke-cat');
    const search = catalog.locator('input').first();
    for (const query of ['测试钻石', 'Fixture Diamond', 'Diamond']) {
      await search.fill(query);
      await expect(catalog.getByRole('option', { name: '测试钻石', exact: true })).toHaveCount(1);
    }
    await page.keyboard.press('Escape');
    if (deck) {
      // Deck dialogs close with their own Back control; Escape is not a controller event.
      if (await catalog.count()) await page.evaluate(() => {
        for (const pressed of [true, false]) (window as any).__kilnMock.emit('kiln://gamepad', { kind: 'button', button: 'b', pressed, repeat: false, device: 'event9' });
      });
    }
    await expect(catalog).toHaveCount(0);
    await (deck ? page.locator('[data-deck-id="ed-tab-skills"]') : page.getByRole('tab').filter({ hasText: zh.editor.tabs.skills })).click();
    if (deck) await expect(page.locator('[data-deck-id="node-29426941"]')).toHaveAttribute('aria-label', /测试钩爪/);
    else await expect(page.locator('.ke-node[aria-label*="测试钩爪"]')).toHaveCount(1);
    await (deck ? page.locator('[data-deck-id="ed-tab-know"]') : page.getByRole('tab').filter({ hasText: zh.editor.tabs.knowledge })).click();
    const knowledgeSearch = page.locator('[data-deck-id="know-search"]');
    for (const query of ['测试配方', 'Fixture Recipe']) {
      await knowledgeSearch.fill(query);
      await expect(page.locator('.ke-know-body [role="row"]')).toHaveCount(1);
      await expect(page.locator('.ke-know-body')).toContainText('测试配方');
    }
    await context.close();
  });
}

for (const extracted of [false, true]) {
  test(`desktop: ${extracted ? 'missing Chinese uses extracted English' : 'no extracted names keeps existing English'}`, async ({ browser }) => {
    const context = await browser.newContext({ locale: 'zh-CN' });
    const page = await context.newPage();
    await page.addInitScript(() => Object.defineProperty(navigator, 'languages', { get: () => ['zh-CN'] }));
    await page.route('**/assets-local/editor/names/names.json*', (route) => extracted
      ? route.fulfill({ json: { format: 1, languages: { 'en-us': names.languages['en-us'] } } })
      : route.fulfill({ status: 404, body: '' }));
    await page.goto('/#/editor');
    await expect(page.locator('.ke .k-page-title')).toContainText('Aldric', { timeout: 15000 });
    const name = extracted ? 'Fixture Diamond' : 'Diamond';
    await expect(page.locator('[data-deck-id="slot-main:0"]')).toHaveAttribute('aria-label', new RegExp(name));
    await page.locator('[data-deck-id="slot-main:0"]').click();
    await expect(page.locator('.ke-insp')).toContainText(name);
    await context.close();
  });
}
