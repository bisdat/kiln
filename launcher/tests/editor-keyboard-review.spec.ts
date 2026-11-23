// Synthetic saves only. Independent keyboard regressions and serialized-save invariants.
import { expect, test, type Page } from '@playwright/test';

const slot = (page: Page, key: string) => page.locator(`.ke-grid [data-deck-id="slot-${key}"]`);
async function open(page: Page) {
  await page.goto('/#/editor');
  await expect(page.locator('.ke .k-page-title')).toContainText('Aldric', { timeout: 15000 });
}
// Hold animation callbacks so a second key deterministically arrives before the next paint.
async function holdFrames(page: Page) {
  await page.evaluate(() => {
    const w = window as any;
    const original = window.requestAnimationFrame;
    const pending: FrameRequestCallback[] = [];
    w.requestAnimationFrame = (cb: FrameRequestCallback) => { pending.push(cb); return -pending.length; };
    w.releaseReviewFrames = () => {
      w.requestAnimationFrame = original;
      for (const cb of pending) cb(performance.now());
    };
  });
}
const releaseFrames = (page: Page) => page.evaluate(() => (window as any).releaseReviewFrames());

// Build an independent expected SV2C file from the bytes read BEFORE the UI edits, not from the resulting model.
async function expectSave(page: Page, edits: [string, string][], copies: [string, string][] = []) {
  await page.evaluate(async ({ swaps, copies }) => {
    const load = (path: string) => import(/* @vite-ignore */ path);
    const [{ useSession }, commands, sv2c, character, data] = await Promise.all([
      load('/src/editor/kiln/session.js'), load('/src/api/commands.ts'), load('/src/editor/shared/sv2c-web.mjs'),
      load('/src/editor/shared/character.mjs'), load('/src/editor/src/gamedata.js'),
    ]);
    const path = useSession.getState().model.path;
    const bytes = new Uint8Array(await commands.editorReadFile(path));
    await sv2c.init();
    const save = await sv2c.parse(bytes, () => { throw Error('Unexpected compression'); }, ['player_character']);
    const pc = save.blocks.find((b: any) => b.name === 'player_character');
    const worn = new Set(['Armour_Head', 'Armour_UpperBody', 'Armour_Arms', 'Armour_LowerBody', 'Armour_Feet', 'Armour_Ring', 'Shield', 'HookShot', 'Glider', 'Backpack']);
    const c = character.parseCharacter(pc.payload, (id: number) => !!data.items[id], (id: number) => !!data.skills.nodes[id], (id: number) => worn.has(data.items[id]?.slot));
    for (const [from, to] of swaps) {
      const [ac, ai] = from.split(':'), [bc, bi] = to.split(':');
      const a = c[ac].find((e: any) => e.slot === Number(ai));
      const b = c[bc].find((e: any) => e.slot === Number(bi));
      c[ac] = c[ac].filter((e: any) => e.slot !== Number(ai));
      c[bc] = c[bc].filter((e: any) => e.slot !== Number(bi));
      if (a) c[bc].push({ ...a, slot: Number(bi) });
      if (b) c[ac].push({ ...b, slot: Number(ai) });
      for (const container of ['main', 'extra']) c[container].sort((a: any, b: any) => a.slot - b.slot);
    }
    for (const [from, to] of copies) {
      const [ac, ai] = from.split(':'), [bc, bi] = to.split(':');
      c[bc].push({ ...c[ac].find((e: any) => e.slot === Number(ai)), slot: Number(bi) });
      c[bc].sort((a: any, b: any) => a.slot - b.slot);
    }
    const payload = character.buildCharacter(c);
    const expected = await sv2c.build({ header: save.header, blocks: save.blocks.map((b: any) => b === pc ? { ...b, payload, changed: true } : b) }, () => { throw Error('Unexpected compression'); });
    (window as any).reviewSave = { path, before: Array.from(bytes), expected: Array.from(expected) };
  }, { swaps: edits, copies });
}
async function checkSavedBytes(page: Page, save = true) {
  if (save) {
    await page.keyboard.press('Control+s');
    await expect.poll(() => page.evaluate(async () => {
      const path = '/src/editor/kiln/session.js';
      const { useSession } = await import(/* @vite-ignore */ path);
      const { saving, dirty } = useSession.getState();
      return { saving, dirty };
    })).toEqual({ saving: false, dirty: false });
  }
  const { actual, expected } = await page.evaluate(async () => {
    const load = (path: string) => import(/* @vite-ignore */ path);
    const commands = await load('/src/api/commands.ts');
    const proof = (window as any).reviewSave;
    return { actual: Array.from(new Uint8Array(await commands.editorReadFile(proof.path))), expected: proof.expected };
  });
  expect(actual).toEqual(expected);
}

test('equipment Enter enters the inspector before a following key', async ({ page }) => {
  await open(page);
  await page.getByRole('tab', { name: 'Equipment', exact: true }).click();
  await page.locator('[data-deck-id="worn-5"]').focus();
  await holdFrames(page);
  await page.keyboard.press('Enter');
  const inInspector = await page.evaluate(() => !!document.activeElement?.closest('.ke-insp'));
  await releaseFrames(page);
  expect(inInspector).toBe(true);
});

test('equipment removal returns focus before the next Tab', async ({ page }) => {
  await open(page);
  await page.getByRole('tab', { name: 'Equipment', exact: true }).click();
  await page.locator('[data-deck-id="worn-5"]').focus();
  await holdFrames(page);
  await page.locator('.ke-insp [data-deck-id="item-remove"]').click();
  await page.keyboard.press('Tab');
  await releaseFrames(page);
  await expect(page.locator('[data-deck-id="worn-6"]')).toBeFocused();
});

test('Delete followed by an arrow does not restore stale slot focus on the next frame', async ({ page }) => {
  await open(page);
  await slot(page, 'main:0').focus();
  await holdFrames(page);
  await page.keyboard.press('Delete');
  await page.keyboard.press('ArrowRight');
  await expect(slot(page, 'main:1')).toBeFocused();
  await releaseFrames(page);
  await expect(slot(page, 'main:1')).toBeFocused();
});

test('duplicate then arrow and Delete cannot remove an unrelated stack before the next frame', async ({ page }) => {
  await open(page);
  await expectSave(page, [], [['main:0', 'main:10']]);
  await slot(page, 'main:0').focus();
  await holdFrames(page);
  await page.keyboard.press('Control+d');
  await page.keyboard.press('ArrowRight');
  await page.keyboard.press('Delete');
  await releaseFrames(page);
  await expect.soft(slot(page, 'main:1')).toHaveAccessibleName(/Metal Scraps.*250/);
  await expect(slot(page, 'main:10')).toHaveAccessibleName(/Diamond.*120/);
  await checkSavedBytes(page);
});

test('keyboard swap across backpack containers and move to action bar preserve complete serialized bytes', async ({ page }) => {
  await open(page);
  await expectSave(page, [['main:24', 'extra:0'], ['extra:5', 'main:27']]);
  await slot(page, 'main:24').focus();
  await page.keyboard.press('m');
  await slot(page, 'extra:0').focus();
  await page.keyboard.press('Enter');
  await expect(slot(page, 'extra:0')).toHaveAccessibleName(/Eclipsed Blade/);
  await slot(page, 'extra:5').focus();
  await page.keyboard.press('m');
  await slot(page, 'main:27').focus();
  await page.keyboard.press('Space');
  await expect(slot(page, 'main:27')).toHaveAccessibleName(/Wolf/);
  await checkSavedBytes(page);
});

async function useGameBackpackHeader(page: Page, empty: boolean) {
  await page.evaluate(async (empty) => {
    const load = (path: string) => import(/* @vite-ignore */ path);
    const [session, commands, sv2c, character, data] = await Promise.all([
      load('/src/editor/kiln/session.js'), load('/src/api/commands.ts'), load('/src/editor/shared/sv2c-web.mjs'),
      load('/src/editor/shared/character.mjs'), load('/src/editor/src/gamedata.js'),
    ]);
    const path = session.useSession.getState().model.path;
    const bytes = new Uint8Array(await commands.editorReadFile(path));
    await sv2c.init();
    const save = await sv2c.parse(bytes, () => { throw Error('Unexpected compression'); }, ['player_character']);
    const pc = save.blocks.find((b: any) => b.name === 'player_character');
    const c = character.parseCharacter(pc.payload, (id: number) => !!data.items[id], (id: number) => !!data.skills.nodes[id]);
    // The worn backpack has 32 slots: its page count is four even when it has no entries.
    c._raw.between[c._raw.between.length - 2] = 4;
    if (empty) c.extra = [];
    const payload = character.buildCharacter(c);
    const changed = await sv2c.build({ header: save.header, blocks: save.blocks.map((b: any) => b === pc ? { ...b, payload, changed: true } : b) }, () => { throw Error('Unexpected compression'); });
    await commands.editorWriteSave(path, Array.from(changed));
    await session.openSave(path);
  }, empty);
}

for (const empty of [false, true]) test(`game-shaped ${empty ? 'empty' : 'occupied'} backpack supports its last slot`, async ({ page }) => {
  await open(page);
  await useGameBackpackHeader(page, empty);
  await expectSave(page, [['main:0', 'extra:31']]);
  await slot(page, 'main:0').focus();
  await page.keyboard.press('m');
  await slot(page, 'extra:31').focus();
  await page.keyboard.press('Enter');
  await checkSavedBytes(page);
  await expect(slot(page, 'extra:31')).toHaveAccessibleName(/Diamond.*120/);
  await expect(page.locator('.ke-group-head .k-card-sub').first()).toContainText('56');
});

test('last displayed extra-container slot remains writable and readable after a keyboard move', async ({ page }) => {
  await open(page);
  const last = page.locator('.ke-grid [data-deck-id^="slot-extra:"]').last();
  const key = (await last.getAttribute('data-deck-id'))!.slice(5);
  await expectSave(page, [['main:0', key]]);
  await slot(page, 'main:0').focus();
  await page.keyboard.press('m');
  await last.focus();
  await page.keyboard.press('Enter');
  await checkSavedBytes(page);
  await expect(last).toHaveAccessibleName(/Diamond.*120/);
  const error = await page.evaluate(async () => {
    const path = '/src/editor/kiln/session.js';
    const { useSession } = await import(/* @vite-ignore */ path);
    return useSession.getState().model.error;
  });
  expect(error).toBeFalsy();
});

test('cancel and self-drop do not alter a save, and equipment is not a move destination', async ({ page }) => {
  await open(page);
  await expectSave(page, []);
  await slot(page, 'extra:5').focus();
  await page.keyboard.press('m');
  await page.keyboard.press('ArrowLeft');
  await page.keyboard.press('Escape');
  await expect(slot(page, 'extra:5')).toBeFocused();
  await page.keyboard.press('m');
  await page.keyboard.press('Enter');
  await expect(page.locator('.ke .k-page-sub')).not.toContainText('Changes not saved');
  await page.keyboard.press('m');
  await page.getByRole('tab', { name: 'Equipment', exact: true }).click();
  await page.locator('[data-deck-id="worn-5"]').focus();
  await page.keyboard.press('Enter');
  const dirty = await page.evaluate(async () => {
    const path = '/src/editor/kiln/session.js';
    const { useSession } = await import(/* @vite-ignore */ path);
    return useSession.getState().dirty;
  });
  expect(dirty).toBe(false);
  await checkSavedBytes(page, false);
});

test('read-only inventory equipment and skill inputs cannot change the model or stored bytes', async ({ page }) => {
  await open(page);
  await page.locator('.ke-who-btn').click();
  await page.getByRole('option', { name: /Mordred/ }).click();
  await expect(page.locator('.ke .k-page-title')).toContainText('Mordred');
  const before = await page.evaluate(async () => {
    const load = (path: string) => import(/* @vite-ignore */ path);
    const [{ useSession }, commands] = await Promise.all([load('/src/editor/kiln/session.js'), load('/src/api/commands.ts')]);
    const model = useSession.getState().model;
    const bytes = Array.from(new Uint8Array(await commands.editorReadFile(model.path)));
    (window as any).reviewSave = { path: model.path, expected: bytes };
    return JSON.stringify(model);
  });
  await slot(page, 'main:0').focus();
  for (const key of ['Control+d', 'Delete', 'm', 'i', 'Enter']) await page.keyboard.press(key);
  await page.locator('.ke-insp [aria-label="+1"]').first().focus();
  await page.keyboard.press('Enter');
  await page.getByRole('tab', { name: 'Equipment', exact: true }).click();
  await page.locator('.ke-facts .ke-step [aria-label="+1"]').first().focus();
  await page.keyboard.press('Enter');
  await page.locator('[data-deck-id="worn-5"]').focus();
  await page.locator('.ke-insp [role="radio"]').first().focus();
  await page.keyboard.press('Enter');
  await page.locator('.ke-insp [data-deck-id="item-remove"]').focus();
  await page.keyboard.press('Enter');
  await page.getByRole('tab', { name: 'Skills', exact: true }).click();
  const node = page.getByRole('group', { name: 'Skill tree', exact: true }).getByRole('button', { name: /^Mage Apprentice/ });
  await node.focus();
  for (const key of ['Enter', 'Space', '-', 'Delete']) await page.keyboard.press(key);
  const after = await page.evaluate(async () => {
    const path = '/src/editor/kiln/session.js';
    const { useSession } = await import(/* @vite-ignore */ path);
    return JSON.stringify(useSession.getState().model);
  });
  expect(after).toBe(before);
  await checkSavedBytes(page, false);
  await expect(page.getByRole('button', { name: 'Save', exact: true })).toBeDisabled();
});
