import { expect, test } from 'bun:test';
import { readFileSync } from 'node:fs';
// @ts-ignore
import { encodeWitness, inventoryWitness } from '../src/editor/shared/storage-witness.mjs';
const vectors = JSON.parse(readFileSync(new URL('../../tests/fixtures/player-storage/witness-vectors.json', import.meta.url), 'utf8'));
for (const v of vectors.vectors) test(`witness vector: ${v.name}`, async () => {
  expect(encodeWitness(v.fields).toString('hex')).toBe(v.encodedHex);
  expect(await inventoryWitness(v.fields)).toBe(v.witness);
});
test('only inventory fields and present option values contribute', async () => {
  const empty = vectors.vectors[0].fields;
  expect(await inventoryWitness({ ...empty, level: 99, xp: 300, _tail: Buffer.alloc(22, 9), storage: 'excluded' })).toBe(vectors.vectors[0].witness);
  expect(await inventoryWitness({ ...empty, mainPages: 1 })).not.toBe(vectors.vectors[0].witness);
});
test('each concurrent call has independent hash state', async () => {
  const results = await Promise.all(vectors.vectors.map((v: any) => inventoryWitness(v.fields)));
  expect(results).toEqual(vectors.vectors.map((v: any) => v.witness));
});
