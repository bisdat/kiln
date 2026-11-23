import { afterEach, expect, test } from 'bun:test';
import { registerLocale, setLanguage } from '../src/i18n';
import { itemName, skillName, nomeConhecimento, items } from '../src/editor/src/gamedata.js';
import { setGameNames, gameName, gameLanguage } from '../src/editor/kiln/game-names.js';
// @ts-ignore: mounted editor components are plain JavaScript.
import { allItems, filterItems } from '../src/editor/kiln/Catalog.jsx';

const table = { format: 1, languages: {
  'en-us': { items: { '61577244': 'Game Diamond' }, skills: { '29426941': 'Game Hook' }, knowledge: { '40769': 'Game Recipe' } },
  'zh-cn': { items: { '61577244': '测试钻石' }, skills: { '29426941': '测试钩爪' }, knowledge: { '40769': '测试配方' } },
  'de-de': { items: { '61577244': 'Testdiamant' } },
} };
afterEach(() => { setGameNames(null); setLanguage('en'); });

test('maps all launcher locales and preserves regional Chinese and Portuguese', () => {
  for (const [from, to] of Object.entries({ en: 'en-us', de: 'de-de', fr: 'fr-fr', it: 'it-it', es: 'es-es', ru: 'ru-ru', pl: 'pl-pl', ja: 'ja-jp', ko: 'ko-kr', 'pt-BR': 'pt-br', 'zh-CN': 'zh-cn', 'zh-TW': 'zh-tw', 'pt-PT': 'pt-pt' })) expect(gameLanguage(from)).toBe(to);
});

test('selects game names for items, skill nodes and knowledge in the launcher language', () => {
  registerLocale('zh-CN', {}); setLanguage('zh-CN'); setGameNames(table);
  expect(itemName(61577244)).toBe('测试钻石');
  expect(skillName(29426941)).toBe('测试钩爪');
  expect(nomeConhecimento({ t: 'r', i: 61577244 }, 40769)).toBe('测试配方');
  expect(nomeConhecimento({ t: 'i', i: 61577244 }, 123)).toBe('测试钻石');
});

test('falls back to game English, then the existing table, including without extraction', () => {
  registerLocale('de', {}); setLanguage('de'); setGameNames(table);
  expect(itemName(61577244)).toBe('Testdiamant');
  expect(skillName(29426941)).toBe('Game Hook');
  expect(itemName(3087876)).toBe(items[3087876].en);
  expect(nomeConhecimento({ n: 'old flag' }, 42)).toBe('old flag');
  setGameNames(null);
  expect(itemName(61577244)).toBe(items[61577244].en);
});

test('catalog matches width variants and Latin accents without changing displayed names', () => {
  registerLocale('ja', {}); setLanguage('ja');
  setGameNames({ format: 1, languages: {
    'ja-jp': { items: { 61577244: 'テスト ＡＢＣ ｶﾀｶﾅ' } },
    'en-us': { items: { 61577244: 'Fixture Café' } },
  } });
  const all = allItems();
  for (const q of ['abc', 'カタカナ', 'cafe', 'CAFÉ']) {
    expect(filterItems(all, { q }).map((it: { id: number }) => it.id)).toContain(61577244);
  }
  expect(all.find((it: { id: number }) => it.id === 61577244).name).toBe('テスト ＡＢＣ ｶﾀｶﾅ');
});

test('missing, blank and invalid tables do not displace fallback names', () => {
  setGameNames({ format: 1, languages: { 'zh-cn': { items: { 1: '  ' } }, 'en-us': { items: { 1: 'English' } } } });
  expect(gameName('items', 1, 'zh-CN')).toBe('English');
  expect(gameName('items', 99, 'zh-CN')).toBeUndefined();
  setGameNames({ format: 99, languages: table.languages });
  expect(gameName('items', 61577244, 'zh-CN')).toBeUndefined();
});
