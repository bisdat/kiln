import { expect, test } from 'bun:test';
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const root = join(import.meta.dir, '../src/i18n');
const english = JSON.parse(readFileSync(join(root, 'en.json'), 'utf8')).playerStorage;
for (const file of readdirSync(root).filter((name) => name.endsWith('.json') && name !== 'en.json')) {
  test(`${file} translates every player storage text`, () => {
    const table = JSON.parse(readFileSync(join(root, file), 'utf8')).playerStorage ?? {};
    expect(Object.keys(table).sort()).toEqual(Object.keys(english).sort());
    for (const value of Object.values(table)) expect(typeof value === 'string' && value.trim().length > 0).toBe(true);
  });
}
