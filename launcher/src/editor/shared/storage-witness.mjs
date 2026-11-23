// SPDX-License-Identifier: MIT
// KPS1 canonical inventory witness; docs/re/player-storage.md, "Witness encoding".
import { Buffer } from 'buffer';
import { createXXHash3 } from 'hash-wasm';
import { inventoryMask, inventoryHeader, recordOptions, uintBytes } from './character-exact.mjs';

function record(e) {
  return Buffer.concat([uintBytes(e.slot, 1, 'slot'), uintBytes(e.itemId, 4, 'item id'), uintBytes(e.count, 2, 'count'),
    ...recordOptions(e).flatMap((bytes) => bytes ? [Buffer.from([1]), bytes] : [Buffer.from([0])])]);
}
export function encodeWitness(c) {
  const mask = inventoryMask(c);
  return Buffer.concat([Buffer.from('KPS1', 'ascii'), uintBytes(mask, 4),
    ...c.equipment.flatMap((e) => e != null ? [Buffer.from([1]), record(e)] : [Buffer.from([0])]),
    ...c.references.flatMap((r) => r != null ? [Buffer.from([1]), uintBytes(r, 2, 'reference')] : [Buffer.from([0])]),
    inventoryHeader(c), ...c.main.map(record), ...c.extra.map(record)]);
}
export async function inventoryWitness(c) {
  const bytes = encodeWitness(c);
  const hash = await createXXHash3(); // no shared mutable WASM state across concurrent reads
  hash.init(); hash.update(bytes);
  return `xxh3:${hash.digest('hex')}`;
}
