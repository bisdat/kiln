import { expect, test } from 'bun:test';
// @ts-ignore
import { readExactCharacter, writeExactCharacter, writeItemRecord } from '../src/editor/shared/character-exact.mjs';
// @ts-ignore
import { inventoryWitness } from '../src/editor/shared/storage-witness.mjs';
// @ts-ignore
import { storageView, planStorageReturn } from '../src/editor/shared/player-storage.mjs';
import { characterBytes } from './player-character-exact.test';
const id = '11111111-2222-3333-4444-555555555555';
const entry = (s: number, itemId = 0x12345678) => ({ s, item: itemId, n: 2, b64: writeItemRecord({ slot: s, itemId, count: 2, level: 0, durability: 4, maxDurability: 9 }).toString('base64') });
async function fixture(n = 3) {
  const c = readExactCharacter(characterBytes());
  const current = { witness: await inventoryWitness(c), worldRev: 3011, storages: { 'mod:bag': { slots: 64, records: Array.from({ length: n }, (_, i) => entry(i)), refs: { arrow: 0, spell: n - 1 } } }, parked: {} };
  const doc = { v: 1, character: id, revision: 42, current, history: [], quarantine: [{ storage: 'bad:key', record: 'AA==', reason: 'decode' }] };
  const envelope = { format: 1, character: id, revision: 90, mods: { 'other-mod': { keep: { x: 1 } }, 'kiln-items': { storage: doc, unrelated: true } } };
  return { c, envelope, doc };
}
const plan = (c: any, e: any) => planStorageReturn(c, e, id, () => 9999);

test('return adds main pages, retains all record fields, removes returned records and refs, raises revisions', async () => {
  const { c, envelope } = await fixture();
  const result = await plan(c, envelope);
  expect(result.returned).toBe(3); expect(result.remaining).toBe(0);
  expect(result.character.mainPages).toBe(6);
  expect(result.character.main.slice(-3).map((e: any) => e.slot)).toEqual([40, 41, 42]);
  expect(result.character.main.at(-1).durability).toBe(4);
  const doc = result.sidecar.mods['kiln-items'].storage;
  expect(doc.current.storages['mod:bag'].records).toEqual([]);
  expect(doc.current.storages['mod:bag'].refs).toEqual({});
  expect(doc.revision).toBe(43); expect(result.sidecar.revision).toBe(91);
  expect(result.sidecar.mods['other-mod']).toEqual(envelope.mods['other-mod']);
  expect(doc.quarantine).toEqual(envelope.mods['kiln-items'].storage.quarantine);
  expect(writeExactCharacter(result.character)).not.toEqual(writeExactCharacter(c));
});

test('new witness matches the written file; the old historical storage cannot return twice', async () => {
  const { c, envelope } = await fixture();
  const a = await plan(c, envelope);
  const written = readExactCharacter(writeExactCharacter(a.character));
  expect(a.sidecar.mods['kiln-items'].storage.current.witness).toBe(await inventoryWitness(written));
  expect(a.sidecar.mods['kiln-items'].storage.history[0].witness).toBe(await inventoryWitness(c));
  const b = await plan(written, a.sidecar);
  expect(b.returned).toBe(0); expect(b.character.main.length).toBe(a.character.main.length);
});

test('partial room returns what fits; the remainder and its refs stay', async () => {
  const { c, envelope, doc } = await fixture(12);
  c.mainPages = 15;
  doc.current.witness = await inventoryWitness(c);
  const a = await plan(c, envelope);
  expect(a.returned).toBe(8); expect(a.remaining).toBe(4); expect(a.character.mainPages).toBe(16);
  expect(a.sidecar.mods['kiln-items'].storage.current.storages['mod:bag'].records.map((r: any) => r.s)).toEqual([8, 9, 10, 11]);
  expect(a.sidecar.mods['kiln-items'].storage.current.storages['mod:bag'].refs).toEqual({ spell: 11 });
});

test('full page budget returns nothing and never changes sidecar or revision', async () => {
  const { c, envelope, doc } = await fixture(); c.mainPages = 16;
  doc.current.witness = await inventoryWitness(c);
  const before = JSON.stringify(envelope);
  const a = await plan(c, envelope);
  expect(a.returned).toBe(0); expect(a.remaining).toBe(3); expect(JSON.stringify(a.sidecar)).toBe(before);
});

test('combined 248 record budget is independent of extra-page capacity', async () => {
  const { c, envelope, doc } = await fixture(3);
  c.mainPages = 15; c.backpackPages = 16;
  c.main = Array.from({ length: 120 }, (_, slot) => ({ slot, itemId: 1, count: 1 }));
  c.extra = Array.from({ length: 127 }, (_, slot) => ({ slot, itemId: 2, count: 1 }));
  doc.current.witness = await inventoryWitness(c);
  const a = await plan(c, envelope);
  expect(a.returned).toBe(1); expect(a.remaining).toBe(2);
  expect(a.character.main.length + a.character.extra.length).toBe(248);
});

test('paired history and its aliases beat a higher unpaired revision; new current drops aliases', async () => {
  const { c, envelope, doc } = await fixture();
  const old = { ...structuredClone(doc.current), witness: 'xxh3:0000000000000000', revision: 12, pairs: [{ witness: await inventoryWitness(c), revision: 11 }] };
  (doc.history as any[]).push(old); doc.current.witness = 'xxh3:ffffffffffffffff';
  doc.current.storages['mod:bag'].records = [];
  const a = await plan(c, envelope);
  expect(a.returned).toBe(3); expect(a.sidecar.mods['kiln-items'].storage.revision).toBe(43);
  expect(a.sidecar.mods['kiln-items'].storage.current.pairs).toBeUndefined();
});

test('parked records return too; invalid/unknown records remain byte-for-byte', async () => {
  const { c, envelope, doc } = await fixture(1);
  doc.current.parked = { 'off:bag': { slots: 8, records: [entry(0)] } } as any;
  doc.current.storages['mod:bag'].records.push({ ...entry(1), b64: 'broken' });
  doc.current.storages['mod:bag'].records.push(entry(2, 22));
  const a = await planStorageReturn(c, envelope, id, (item: number) => item === 22 ? null : 9999);
  expect(a.returned).toBe(2); expect(a.remaining).toBe(2);
  expect(a.sidecar.mods['kiln-items'].storage.current.storages['mod:bag'].records).toEqual(doc.current.storages['mod:bag'].records.slice(1));
});

test('unknown document versions, mismatched identity, oversized documents and unsafe revisions refuse', async () => {
  for (const change of [(d: any) => d.v = 2, (d: any) => d.character = 'wrong', (d: any) => d.revision = Number.MAX_SAFE_INTEGER, (d: any) => d.padding = 'x'.repeat(65536)]) {
    const { c, envelope, doc } = await fixture(); change(doc);
    await expect(plan(c, envelope)).rejects.toThrow();
  }
});

test('malformed storage and duplicate slots are not silently deleted or returned', async () => {
  const { c, envelope, doc } = await fixture();
  doc.current.storages['mod:bag'].records[1].s = 0;
  await expect(plan(c, envelope)).rejects.toThrow();
});

test('view is read-only, renders mirror fields, isolates bad rows and warns for future versions', async () => {
  const { c, envelope, doc } = await fixture();
  const view = await storageView(c, envelope, id);
  expect(view.hasStorage).toBe(true); expect(view.groups[0].records[0].itemId).toBe(0x12345678);
  expect(view.groups[0].records[0].count).toBe(2); expect(view.readOnly).toBe(true);
  doc.v = 2;
  expect((await storageView(c, envelope, id)).canReturn).toBe(false);
  expect((await storageView(c, envelope, id)).hasStorage).toBe(true);
});

test('sidecar-first crash points, repeat after interruption, and old backup restore lose or duplicate nothing', async () => {
  const { c, envelope } = await fixture();
  const intended = await plan(c, envelope);
  for (const [character, sidecar, expectedStored] of [[c, envelope, 3], [c, intended.sidecar, 3], [intended.character, intended.sidecar, 0]] as const) {
    const visible = await storageView(character, sidecar, id);
    const stored = visible.groups.flatMap((g: any) => g.records).length;
    expect(stored).toBe(expectedStored);
    expect(character.main.length - c.main.length + stored).toBe(3);
  }
  // The failed write left the old file but the new sidecar; a retry must start from full history.
  const retry = await plan(c, intended.sidecar);
  expect(retry.returned).toBe(3);
  expect(retry.character.main.length).toBe(intended.character.main.length);
  expect((await plan(retry.character, retry.sidecar)).returned).toBe(0);
  // Restoring the original character later selects its full historical storage, not reduced current.
  expect((await storageView(c, retry.sidecar, id)).groups[0].records).toHaveLength(3);
});

test('a character that pairs with no storage copy refuses, changes nothing and cannot be returned from the view', async () => {
  const { c, envelope, doc } = await fixture();
  doc.current.witness = 'xxh3:ffffffffffffffff';
  doc.history = Array.from({ length: 8 }, (_, i) => ({ ...structuredClone(doc.current), witness: `xxh3:${i.toString(16).padStart(16, '0')}`, revision: i })) as any;
  const before = JSON.stringify(envelope), bytes = writeExactCharacter(c);
  await expect(plan(c, envelope)).rejects.toThrow('This character changed since Kiln last saw its storage');
  expect(JSON.stringify(envelope)).toBe(before);
  expect(writeExactCharacter(c)).toEqual(bytes);
  const view = await storageView(c, envelope, id);
  expect(view.paired).toBe(false); expect(view.canReturn).toBe(false);
  expect(view.groups[0].records).toHaveLength(3);
});

test('a character paired by the oldest history pair gets that full pair pinned first, even when history is full', async () => {
  const { c, envelope, doc } = await fixture();
  doc.current.witness = 'xxh3:ffffffffffffffff';
  doc.history = Array.from({ length: 8 }, (_, i) => ({ ...structuredClone(doc.current), witness: `xxh3:${i.toString(16).padStart(16, '0')}`, revision: 8 - i })) as any;
  (doc.history[7] as any).witness = await inventoryWitness(c);
  const a = await plan(c, envelope);
  expect(a.sidecar.mods['kiln-items'].storage.history[0].witness).toBe(await inventoryWitness(c));
  expect(a.sidecar.mods['kiln-items'].storage.history).toHaveLength(8);
  const beforeCharacterWrite = await storageView(c, a.sidecar, id);
  expect(beforeCharacterWrite.paired).toBe(true);
  expect(beforeCharacterWrite.groups[0].records).toHaveLength(3);
});

test('every return that moves a stack adds new main pages and never fills empty cells of existing pages', async () => {
  for (const [n, pages] of [[1, 1], [8, 1], [9, 2]]) {
    const { c, envelope } = await fixture(n);
    expect(c.main.length).toBeLessThan(c.mainPages * 8); // the existing pages have empty cells
    const a = await plan(c, envelope);
    expect(a.returned).toBe(n);
    expect(a.character.mainPages).toBe(c.mainPages + pages);
    expect(a.character.main.slice(0, c.main.length)).toEqual(c.main);
    expect(a.character.main.slice(c.main.length).map((r: any) => r.slot)).toEqual(Array.from({ length: n }, (_, i) => c.mainPages * 8 + i));
    const written = readExactCharacter(writeExactCharacter(a.character));
    expect(written.mainPages).toBeGreaterThan(c.mainPages);
  }
});

const marker = (revision: number, n: number) => ({ revision, fromWitness: `xxh3:${n.toString(16).padStart(16, '0')}`, toWitness: `xxh3:${(n + 1).toString(16).padStart(16, '0')}` });

test('a return records its marker in the same sidecar: new revision, actual old witness, written witness', async () => {
  const { c, envelope, doc } = await fixture();
  (doc as any).returns = [marker(7, 0xa0)];
  const a = await plan(c, envelope);
  const next = a.sidecar.mods['kiln-items'].storage;
  const written = readExactCharacter(writeExactCharacter(a.character));
  expect(next.returns[0]).toEqual({ revision: next.revision, fromWitness: await inventoryWitness(c), toWitness: await inventoryWitness(written) });
  expect(next.returns.slice(1)).toEqual([marker(7, 0xa0)]);
  expect(next.current.witness).toBe(next.returns[0].toWitness);
  // A retry after an interrupted character write keeps the first marker too.
  const retry = await plan(c, a.sidecar);
  const again = retry.sidecar.mods['kiln-items'].storage;
  expect(again.returns.map((m: any) => m.revision)).toEqual([again.revision, next.revision, 7]);
});

test('markers keep the newest 8, newest first, and the new marker outranks every older marker revision', async () => {
  const { c, envelope, doc } = await fixture();
  (doc as any).returns = [103, 100, 107, 101, 106, 102, 105, 104].map((r) => marker(r, r));
  const a = await plan(c, envelope);
  const next = a.sidecar.mods['kiln-items'].storage;
  expect(next.revision).toBe(108);
  expect(next.returns.map((m: any) => m.revision)).toEqual([108, 107, 106, 105, 104, 103, 102, 101]);
  expect(next.current.witness).toBe(await inventoryWitness(a.character));
});

test('byte trimming drops old history pairs but never a return marker', async () => {
  const { c, envelope, doc } = await fixture();
  (doc as any).returns = Array.from({ length: 7 }, (_, i) => marker(i + 1, i));
  (doc.current as any).padding = 'c'.repeat(10000);
  doc.history = [
    { ...structuredClone(doc.current), witness: 'xxh3:0000000000000002', revision: 2, padding: 'b'.repeat(20000) },
    { ...structuredClone(doc.current), witness: 'xxh3:0000000000000000', revision: 0, padding: 'a'.repeat(26000) },
  ] as any;
  expect(Buffer.byteLength(JSON.stringify(doc))).toBeLessThanOrEqual(65536);
  const a = await plan(c, envelope);
  const next = a.sidecar.mods['kiln-items'].storage;
  expect(Buffer.byteLength(JSON.stringify(next))).toBeLessThanOrEqual(65536);
  expect(next.history.some((e: any) => e.witness === 'xxh3:0000000000000000')).toBe(false);
  expect(next.returns).toHaveLength(8);
  expect(next.returns.slice(1)).toEqual((doc as any).returns.slice().reverse());
});

test('malformed return markers refuse instead of being dropped', async () => {
  for (const returns of ['x', [marker(0, 1)], [{ revision: 3, fromWitness: 'same', toWitness: 'same' }], [{ revision: 3, fromWitness: '', toWitness: 'b' }], [{ revision: 3, fromWitness: 'a'.repeat(65), toWitness: 'b' }], [null]]) {
    const { c, envelope, doc } = await fixture();
    (doc as any).returns = returns;
    await expect(plan(c, envelope)).rejects.toThrow('Invalid storage return marker');
  }
});

test('return trims oldest historical pairs to fit 64 KiB while protecting the full pre-return entry', async () => {
  const { c, envelope, doc } = await fixture();
  (doc.current as any).padding = 'c'.repeat(10000);
  doc.history = [
    { ...structuredClone(doc.current), witness: 'xxh3:0000000000000002', revision: 2, padding: 'b'.repeat(20000),
      pairs: [{ witness: 'xxh3:0000000000000001', revision: 1 }] },
    { ...structuredClone(doc.current), witness: 'xxh3:0000000000000000', revision: 0, padding: 'a'.repeat(27000) },
  ] as any;
  expect(Buffer.byteLength(JSON.stringify(doc))).toBeLessThanOrEqual(65536);
  const a = await plan(c, envelope);
  const next = a.sidecar.mods['kiln-items'].storage;
  expect(a.returned).toBe(3);
  expect(Buffer.byteLength(JSON.stringify(next))).toBeLessThanOrEqual(65536);
  expect(next.history[0].witness).toBe(await inventoryWitness(c));
  expect(next.history[0].storages).toEqual(doc.current.storages);
  expect(next.history[0].padding).toBe(doc.current.padding);
  expect(next.history.some((e: any) => e.witness === 'xxh3:0000000000000002')).toBe(true);
  expect(next.history.some((e: any) => e.witness === 'xxh3:0000000000000000')).toBe(false);
  expect((await storageView(c, a.sidecar, id)).groups[0].records).toHaveLength(3);
});

test('oversized current plus the protected pre-return entry refuses without mutating inputs', async () => {
  const { c, envelope, doc } = await fixture();
  (doc.current as any).padding = 'x'.repeat(33000);
  const before = JSON.stringify(envelope), bytes = writeExactCharacter(c);
  await expect(plan(c, envelope)).rejects.toThrow('too large');
  expect(JSON.stringify(envelope)).toBe(before);
  expect(writeExactCharacter(c)).toEqual(bytes);
});

test('source metadata gates the server-copy warning; worldRev alone is not remote provenance', async () => {
  const { c, envelope, doc } = await fixture();
  expect((await storageView(c, envelope, id)).serverCopy).toBe(false);
  (doc.current as any).source = 'local';
  expect((await storageView(c, envelope, id)).serverCopy).toBe(false);
  doc.history = [{ ...structuredClone(doc.current), witness: 'xxh3:0000000000000000', revision: 1, source: 'server' }] as any;
  expect((await storageView(c, envelope, id)).serverCopy).toBe(true);
});

test('malformed decoded mirror values never become React object children or hide readable records', async () => {
  const { c, envelope, doc } = await fixture();
  (doc.current.storages['mod:bag'].records[0] as any).n = { invalid: true };
  const view = await storageView(c, envelope, id);
  expect(view.groups[0].records).toHaveLength(2);
  expect(view.invalidRecords).toBe(1);
});

test('mixed storage returns base-game items only; overlay-only items stay in the new paired storage', async () => {
  // @ts-ignore
  const { vanillaStackLimit } = await import('../src/editor/shared/storage-catalog.mjs');
  const { default: catalog } = await import('../src/editor/data/items.json');
  const modId = 0x7f00abcd;
  const old = (catalog as any)[modId];
  (catalog as any)[modId] = { maxStack: 99, modded: true };
  try {
    const vanilla = Number(Object.keys(catalog).find((id) => vanillaStackLimit(Number(id)) >= 2));
    const { c, envelope, doc } = await fixture(0);
    doc.current.storages['mod:bag'].records = [entry(0, vanilla), entry(1, modId)];
    const a = await planStorageReturn(c, envelope, id, vanillaStackLimit);
    expect(a.returned).toBe(1); expect(a.remaining).toBe(1); expect(a.modItems).toBe(1);
    expect(a.sidecar.mods['kiln-items'].storage.current.storages['mod:bag'].records).toEqual([entry(1, modId)]);
    expect(a.sidecar.mods['kiln-items'].storage.current.witness).toBe(await inventoryWitness(a.character));
    expect((await storageView(a.character, a.sidecar, id)).groups[0].records[0].itemId).toBe(modId);
    const repeat = await planStorageReturn(a.character, a.sidecar, id, vanillaStackLimit);
    expect(repeat.returned).toBe(0); expect(repeat.modItems).toBe(1);
  } finally { if (old) (catalog as any)[modId] = old; else delete (catalog as any)[modId]; }
});
