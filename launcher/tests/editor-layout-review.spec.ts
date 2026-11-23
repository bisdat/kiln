import { expect, test, type Locator, type Page } from '@playwright/test';

const locales = ['en', 'de', 'es', 'fr', 'it', 'ja', 'ko', 'pl', 'pt-BR', 'ru', 'zh-CN'];
const sizes = [{ width: 1024, height: 640 }, { width: 1280, height: 720 }, { width: 1920, height: 1080 }, { width: 1280, height: 800 }];
async function rest(page: Page) {
  await page.evaluate(async () => {
    await document.fonts.ready;
    await Promise.all(document.getAnimations().filter(a => a.effect?.getTiming().iterations !== Infinity).map(a => a.finished.catch(() => {})));
  });
}
async function box(el: Locator, parent?: Locator, vertical = true) {
  const root = parent ? await parent.elementHandle() : null;
  // Focus can reflow the controller hints. Related boxes must describe the same layout state.
  const { r, p } = await el.evaluate((node, root) => ({
    r: node.getBoundingClientRect().toJSON(),
    p: root ? root.getBoundingClientRect().toJSON() : { x: 0, y: 0, width: innerWidth, height: innerHeight },
  }), root);
  await root?.dispose();
  expect.soft(r.width, `visible width for ${el}`).toBeGreaterThan(0);
  expect.soft(r.height, `visible height for ${el}`).toBeGreaterThan(0);
  expect.soft(r.x, `${el} left`).toBeGreaterThanOrEqual(p.x - 1);
  expect.soft(r.x + r.width, `${el} right`).toBeLessThanOrEqual(p.x + p.width + 1);
  if (vertical) {
    expect.soft(r.y, `${el} top`).toBeGreaterThanOrEqual(p.y - 1);
    expect.soft(r.y + r.height, `${el} bottom`).toBeLessThanOrEqual(p.y + p.height + 1);
  }
}
async function ring(el: Locator) {
  await el.page().keyboard.press('Tab');
  await el.focus(); await el.scrollIntoViewIfNeeded();
  const result = await el.evaluate(el => {
    const s = getComputedStyle(el), r = el.getBoundingClientRect();
    const ring = parseFloat(s.outlineWidth) + parseFloat(s.outlineOffset);
    const clips = [];
    for (let p = el.parentElement; p; p = p.parentElement) {
      const s = getComputedStyle(p), b = p.getBoundingClientRect();
      if (['auto', 'hidden', 'clip', 'scroll'].includes(s.overflowX) && (r.left - ring < b.left - 1 || r.right + ring > b.right + 1)) clips.push(p.className + ' horizontal');
      if (['auto', 'hidden', 'clip', 'scroll'].includes(s.overflowY) && (r.top - ring < b.top - 1 || r.bottom + ring > b.bottom + 1)) clips.push(p.className + ' vertical');
    }
    return { outline: s.outlineStyle, ring, clips };
  });
  console.log('FOCUS', await el.getAttribute('class'), JSON.stringify(result));
  expect.soft(result.outline, `visible ring for ${el}`).toBe('solid');
  expect.soft(result.clips, `complete ring for ${el}`).toEqual([]);
}
for (const locale of locales) for (const size of sizes) test(`at-rest long-name missing-art ${locale} ${size.width}x${size.height}`, async ({ page }, info) => {
  const deck = size.height === 800;
  await page.setViewportSize(size);
  await page.route('**/assets-local/editor/**', r => r.abort());
  await page.goto(deck ? '/?deck=steamdeck#/deck/editor' : '/#/editor');
  await expect(page.locator('.ke-grid .ke-slot.full').first()).toBeVisible({ timeout: 15000 });
  await page.evaluate(async locale => {
    (await import('/src/i18n/index.ts')).setLanguage(locale);
    const { useSession } = await import('/src/editor/kiln/session.js');
    useSession.setState({ dirty: true, model: { ...useSession.getState().model, name: 'Хранитель пламени с невероятно длинным именем' } });
  }, locale);
  await rest(page);
  const head = page.locator(deck ? '.dke-head' : '.k-page-head');
  await box(head);
  for (const b of await head.locator('button').all()) await box(b, head);
  const split = page.locator('.ke-split'), main = split.locator('.ke-main'), inspector = split.locator('.ke-insp');
  // A tall title may scroll the desktop sheet; the working well must remain reachable at its full usable height.
  await split.scrollIntoViewIfNeeded();
  await box(split);
  for (const grid of await page.locator('.ke-grid').all()) await box(grid, main, false);
  const scroll = page.locator('.ke-grid-scroll');
  expect.soft(await scroll.evaluate(el => el.clientHeight), 'backpack always has a visible row').toBeGreaterThanOrEqual(deck ? 56 : 64);
  const slot = page.locator('.ke-grid .ke-slot.full').first();
  await slot.click(); await rest(page);
  await box(inspector, split, deck || size.width !== 1024);
  for (const control of await inspector.locator('button, input, select').all()) {
    await control.scrollIntoViewIfNeeded(); await box(control); await box(control, inspector, false);
  }
  if (deck) for (const hint of await page.locator('.deck-hint').all()) await box(hint, page.locator('.deck-hints'));
  else await box(page.locator('.kiln-editor-hint'));
  await page.screenshot({ path: info.outputPath('inventory.png') });
  if (locale === 'ru') {
    await ring(page.locator('.ke-grid .ke-slot').first());
    await ring(page.locator('.ke-grid .ke-slot').nth(7));
    if (deck) await ring(page.locator('.ke-insp .ke-step-v').first());
  }
  await page.locator(deck ? '.dke-tabs button' : '[role="tab"]').nth(2).click(); await rest(page);
  const points = page.locator('.ke-tree-points'), attrs = page.locator('.ke-tree-attrs');
  await page.locator('.ke-tree').scrollIntoViewIfNeeded();
  await box(page.locator('.ke-tree'));
  await box(points, page.locator('.ke-tree'));
  const p = (await points.boundingBox())!, a = (await attrs.boundingBox())!;
  expect.soft(p.y + p.height, 'skill explanation does not overlap attributes').toBeLessThanOrEqual(a.y);
  await page.screenshot({ path: info.outputPath('skills.png') });
  const tabs = page.locator(deck ? '.dke-tabs button' : '[role="tab"]');
  await tabs.nth(0).click(); await rest(page);
  await page.locator('.ke-split').scrollIntoViewIfNeeded();
  await box(page.locator('.ke-gear'), page.locator('.ke-main'), false);
  await tabs.nth(3).click(); await rest(page);
  await page.locator('.ke-know').scrollIntoViewIfNeeded();
  await box(page.locator('.ke-know-bar'), page.locator('.ke-know'));
  await page.locator(deck ? '[data-deck-id="ed-backups"]' : '.k-page-actions button[title="B"]').click(); await rest(page);
  await box(page.getByRole('dialog'));
  if (!deck) for (const row of await page.locator('.ke-bk-row').all()) for (const cell of await row.locator(':scope > span').all()) await box(cell, row);
  await page.keyboard.press('Escape');
  await tabs.nth(1).click(); await rest(page);
  if (deck) await page.locator('.ke-grid .ke-slot:not(.full)').first().click();
  else await page.locator('.ke-group-head button').first().click();
  await expect(page.locator('.ke-cat-grid .ke-slot').first()).toBeVisible(); await rest(page);
  await box(page.getByRole('dialog'));
  await box(page.locator('.ke-cat'), page.getByRole('dialog'));
  for (const panel of await page.locator('.ke-cat > *').all()) await box(panel, page.locator('.ke-cat'));
  await page.locator('.ke-cat-grid .ke-slot').first().click(); await rest(page);
  for (const button of await page.locator('.ke-cat-pick button').all()) await box(button, page.locator('.ke-cat-pick'));
  await page.screenshot({ path: info.outputPath('catalog-selected.png') });
});

test('at-rest Catalog geometry and measured contrast', async ({ page }, info) => {
  await page.goto('/#/editor'); await expect(page.locator('.ke-grid .ke-slot.full').first()).toBeVisible();
  await rest(page); await page.locator('.ke-group-head button').first().click();
  await expect(page.locator('.ke-cat-grid .ke-slot').first()).toBeVisible(); await rest(page);
  const count = page.locator('.ke-cat-rail .k-sub.on .ke-cat-n');
  await box(count, page.locator('.ke-cat-rail'));
  const colors = await count.evaluate(el => {
    const rgb = (s: string) => s.match(/[\d.]+/g)!.map(Number);
    let bg = [0, 0, 0];
    const ps: Element[] = []; for (let p: Element | null = el; p; p = p.parentElement) ps.unshift(p);
    for (const p of ps) { const c = rgb(getComputedStyle(p).backgroundColor), a = c[3] ?? 1; bg = bg.map((v, i) => c[i] * a + v * (1 - a)); }
    const lum = (c: number[]) => c.slice(0, 3).map(x => x / 255).map(x => x <= .04045 ? x / 12.92 : ((x + .055) / 1.055) ** 2.4).reduce((s, x, i) => s + x * [.2126, .7152, .0722][i], 0);
    const fg = rgb(getComputedStyle(el).color), f = lum(fg), b = lum(bg);
    return { fg, bg, contrast: (Math.max(f,b) + .05) / (Math.min(f,b) + .05) };
  });
  console.log('CATALOG_CONTRAST', JSON.stringify(colors));
  expect(colors.contrast).toBeGreaterThanOrEqual(4.5);
  await ring(page.locator('.ke-cat-rail button').first());
  await ring(page.locator('.ke-cat-grid .ke-slot').first());
  await page.screenshot({ path: info.outputPath('catalog.png') });
});

test('Deck Catalog quantity retains its focus ring', async ({ page }, info) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto('/?deck=steamdeck#/deck/editor');
  await expect(page.locator('.ke-grid .ke-slot.full').first()).toBeVisible(); await rest(page);
  await page.locator('.ke-grid .ke-slot:not(.full)').first().click();
  await expect(page.locator('.ke-cat-grid .ke-slot').first()).toBeVisible(); await rest(page);
  await page.locator('.ke-cat-main input').fill('Diamond');
  await page.locator('.ke-cat-grid').getByRole('option', { name: 'Diamond', exact: true }).click();
  await expect(page.locator('.ke-cat-qty .ke-step-v')).toBeVisible(); await rest(page);
  await ring(page.locator('.ke-cat-rail button').first());
  await ring(page.locator('.ke-cat-grid .ke-slot').first());
  await ring(page.locator('.ke-cat-qty .ke-step-v'));
  await page.screenshot({ path: info.outputPath('catalog-quantity.png') });
});
