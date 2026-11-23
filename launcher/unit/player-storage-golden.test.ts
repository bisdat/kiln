import { expect, test } from 'bun:test';
import { readFileSync, writeFileSync } from 'node:fs';
// @ts-ignore: shared browser codec
import { readExactCharacter, writeExactCharacter, writeItemRecord } from '../src/editor/shared/character-exact.mjs';
// @ts-ignore
import { inventoryWitness } from '../src/editor/shared/storage-witness.mjs';
// @ts-ignore
import { planStorageReturn } from '../src/editor/shared/player-storage.mjs';
import { characterBytes } from './player-character-exact.test';

// The offline return's request exactly as the planner makes it, for the Rust disk boundary's golden test
// (crates/kiln-manage editor_storage: return-sidecar.json) and Kiln.Saves' pairing tests. Synthetic data only.
// KILN_UPDATE_FIXTURES=1 rewrites it after an intended change.
const GOLDEN = new URL('../../tests/fixtures/player-storage/return-request.json', import.meta.url);
const id = '12812812-8128-4128-8128-128128128128';
const VANILLA_A = 0x0001e240, VANILLA_B = 0x0001e241, MOD_ITEM = 0x7f000001;

function stack(s: number, itemId: number, count: number, gear = false) {
  const record = { slot: s, itemId, count, ...(gear ? { level: 7, durability: 40, maxDurability: 90, appearance: '0a0b0c0d' } : {}) };
  return { s, b64: writeItemRecord(record).toString('base64'), item: itemId, n: count, ...(gear ? { level: 7, durability: 40, maxDurability: 90, appearance: '0a0b0c0d' } : {}) };
}

export async function returnRequest() {
  const c = readExactCharacter(characterBytes());
  const before = await inventoryWitness(c);
  const storages = { 'quiver-mod:quiver': { slots: 18, records: [stack(0, VANILLA_A, 64), stack(1, MOD_ITEM, 3), stack(2, VANILLA_B, 1, true)], refs: { arrow: 0 } } };
  const older = { witness: 'xxh3:00000000000000a1', worldRev: 3000, storages: { 'quiver-mod:quiver': { slots: 18, records: [stack(0, VANILLA_A, 20)], refs: {} } }, parked: {}, revision: 40, source: 'server' };
  const storage = { v: 1, character: id, revision: 42, current: { witness: before, worldRev: 3011, storages, parked: {}, source: 'server' },
    history: [older], quarantine: [{ storage: 'gone:bag', record: 'AAEAAAABAAA=', raw: '{}', reason: 'unknown item' }],
    returns: [{ revision: 12, fromWitness: 'xxh3:00000000000000b1', toWitness: 'xxh3:00000000000000b2' }] };
  // As Kiln.Saves writes a sidecar: compact JSON, other mods' values as their own raw text (an integer past 2^53, 1.0).
  const expectedSidecar = `{"format":1,"character":"${id}","revision":17,"updatedUnixMs":1763860000000,"world":"world-1",` +
    `"mods":{"other-mod":{"ticks":638712345678901234567,"steam":76561198000000001,"ratio":1.0,"e":1E2},"kiln-items":{"storage":${JSON.stringify(storage)},"note":1.0}}}`;
  const plan = await planStorageReturn(c, JSON.parse(expectedSidecar), id, (item: number) => (item === MOD_ITEM ? undefined : 9999));
  const written = readExactCharacter(writeExactCharacter(plan.character));
  return {
    expectedSidecar,
    storage: JSON.stringify(plan.sidecar.mods['kiln-items'].storage),
    oldWitness: before,
    newWitness: await inventoryWitness(written),
    returned: plan.returned,
    returnedItems: [VANILLA_A, VANILLA_B],
    keptItems: [MOD_ITEM],
  };
}

test('the offline return request golden is what the planner makes now', async () => {
  const request = await returnRequest();
  expect(request.returned).toBe(2);
  expect(request.newWitness).toBe(JSON.parse(request.storage).current.witness);
  const text = JSON.stringify(request, null, 2) + '\n';
  if (process.env.KILN_UPDATE_FIXTURES === '1') writeFileSync(GOLDEN, text);
  expect(readFileSync(GOLDEN, 'utf8')).toBe(text);
});
