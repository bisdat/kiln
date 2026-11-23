import { expect, test } from 'bun:test';
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { zstdDecompressSync } from 'node:zlib';
import items from '../src/editor/data/items.json';
import skills from '../src/editor/data/skills.json';
const equipmentSlots = new Set(['Armour_Head', 'Armour_UpperBody', 'Armour_Arms', 'Armour_LowerBody', 'Armour_Feet', 'Armour_Ring', 'Shield', 'HookShot', 'Glider', 'Backpack']);
// @ts-ignore: shared browser codec
import { readExactCharacter, writeExactCharacter, readItemRecord, writeItemRecord } from '../src/editor/shared/character-exact.mjs';
// @ts-ignore
import { readCharacter, buildCharacter } from '../src/editor/shared/character.mjs';
// @ts-ignore
import * as sv2c from '../src/editor/shared/sv2c-web.mjs';

const u32 = (n: number) => { const b = Buffer.alloc(4); b.writeUInt32LE(n); return b; };
const row = (slot: number, flags = 0) => Buffer.concat([Buffer.from([slot]), u32(0x12345678), Buffer.from([2, 0, flags]), ...(flags ? [Buffer.from([0]), u32(0), u32(99), u32(3), u32(4), Buffer.from('12345678', 'hex'), Buffer.from([5, 0]), Buffer.from('1234567800', 'hex')] : [])]);
// Independent wire bytes, including a buff with all six options (not the 0x78 in-memory stride).
export function characterBytes({ pages = 5, main = [row(0)], extraPages = 4, extra = [row(1)], buffs = false, version = 5 } = {}) {
  const appearance = Buffer.alloc(version >= 5 ? 38 : 37);
  const buff = Buffer.concat([Buffer.alloc(20), Buffer.from([63, 0]), Buffer.alloc(1 + 4 + 4 + 1 + 8 + 52)]);
  return Buffer.concat([appearance, Buffer.alloc(20), u32(3), u32(7), u32(0), u32(buffs ? 1 : 0), ...(buffs ? [buff] : []), u32(0x30021), row(0), row(1, 63), Buffer.from([0, 0, 1, 0]), Buffer.from([pages, main.length, extraPages, extra.length]), ...main, ...extra, Buffer.alloc(22)]);
}

test('exact field traversal, sparse equipment, references and every record option round-trip', () => {
  const bytes = characterBytes({ buffs: true });
  const c = readExactCharacter(bytes, 5);
  expect(c.equipment.length).toBe(16);
  expect(c.equipment[0].itemId).toBe(0x12345678);
  expect(c.equipment[5].durability).toBe(0);
  expect(c.equipment[1]).toBeNull();
  expect(c.references).toEqual([0, 1, null, null]);
  expect(c.mainPages).toBe(5); expect(c.backpackPages).toBe(4);
  expect(c.main).toHaveLength(1); expect(c.extra).toHaveLength(1);
  expect(writeExactCharacter(c)).toEqual(bytes);
});

test('pre-5 appearance layout is explicit, not guessed', () => {
  const bytes = characterBytes({ version: 4 });
  expect(writeExactCharacter(readExactCharacter(bytes, 4))).toEqual(bytes);
  expect(() => readExactCharacter(bytes, 6)).toThrow();
});

test('all truncations and unknown record flags fail closed', () => {
  const bytes = characterBytes({ buffs: true });
  for (let n = 0; n < bytes.length; n++) expect(() => readExactCharacter(bytes.subarray(0, n))).toThrow();
  expect(() => readItemRecord(Buffer.concat([Buffer.alloc(7), Buffer.from([64])]))).toThrow();
  expect(() => readExactCharacter(Buffer.concat([bytes, Buffer.from([0])]))).toThrow();
});

test('engine page and combined-record limits apply on read and write', () => {
  expect(() => readExactCharacter(characterBytes({ pages: 17 }))).toThrow();
  const c = readExactCharacter(characterBytes());
  expect(() => writeExactCharacter({ ...c, backpackPages: 17 })).toThrow();
  expect(() => readExactCharacter(characterBytes({ main: Array.from({ length: 249 }, () => row(0)), extra: [] }))).toThrow();
  expect(() => writeExactCharacter({ ...c, main: Array(248).fill(c.main[0]) })).toThrow();
});

test('record bytes round-trip, explicit zero option differs from absence', () => {
  const bytes = row(7, 63);
  expect(writeItemRecord(readItemRecord(bytes).record)).toEqual(bytes);
  expect(() => writeItemRecord({ slot: 256, itemId: 1, count: 1 })).toThrow();
  expect(() => writeItemRecord({ slot: 0, itemId: 1, count: 1, appearance: '00' })).toThrow();
});

const fixtureRoot = process.env.KILN_CHARACTER_FIXTURES;
function files(root: string): string[] {
  return readdirSync(root, { withFileTypes: true }).flatMap((e) => e.isDirectory() ? files(join(root, e.name)) : /character\d+\.ksd(?:\.backup-\d+)?$/.test(e.name) ? [join(root, e.name)] : []);
}
for (const path of fixtureRoot ? files(fixtureRoot) : ['real character snapshots (KILN_CHARACTER_FIXTURES unset)']) {
  (fixtureRoot ? test : test.skip)(path.slice(fixtureRoot?.length ?? 0), async () => {
    await sv2c.init();
    const save = await sv2c.parse(readFileSync(path), (b: Uint8Array) => zstdDecompressSync(b), ['player_character']);
    const pc = save.blocks.find((b: any) => b.name === 'player_character');
    const c = readExactCharacter(pc.payload, pc.head.readUInt32LE(10));
    const old = readCharacter(pc.payload, (n: number) => Object.hasOwn(items, n), (n: number) => Object.hasOwn(skills.nodes, n), (n: number) => equipmentSlots.has((items as any)[n]?.slot)).character;
    expect(c.equipment.filter(Boolean)).toEqual(old.equipment);
    expect(c.main).toEqual(old.main); expect(c.extra).toEqual(old.extra);
    expect(writeExactCharacter(c)).toEqual(pc.payload);
    expect(buildCharacter(old)).toEqual(pc.payload);
  });
}
