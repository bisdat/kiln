// SPDX-License-Identifier: MIT
// Exact player_character wire codec. The editor's heuristic character.mjs remains unchanged.
// Layout evidence: docs/re/player-storage.md. Inventory list stride in memory is 0x38;
// each of the 16 optional equipment records has a 0x3c stride, including its has-value byte.
import { Buffer } from 'buffer';

const OPTIONS = [
  [['level', 1]],
  [['durability', 4], ['maxDurability', 4]],
  [['liquid', 4], ['maxLiquid', 4]],
  [['appearance', 4, 'hex']],
  [['maxUpgrades', 1], ['upgrades', 1]],
  [['socket', 5, 'hex']],
];

export function integer(n, max, name) {
  if (!Number.isSafeInteger(n) || n < 0 || n > max) throw new Error(`Invalid ${name}`);
  return n;
}
export function uintBytes(n, size, name = 'integer') {
  integer(n, size === 4 ? 0xffffffff : 2 ** (size * 8) - 1, name);
  const b = Buffer.alloc(size);
  b.writeUIntLE(n, 0, size);
  return b;
}
class Reader {
  constructor(bytes) { this.bytes = Buffer.from(bytes); this.offset = 0; }
  take(n) {
    if (!Number.isSafeInteger(n) || n < 0 || n > this.bytes.length - this.offset) throw new Error('Truncated character record');
    const b = this.bytes.subarray(this.offset, this.offset + n); this.offset += n; return b;
  }
  uint(n) { return this.take(n).readUIntLE(0, n); }
}
function readRecord(r) {
  const e = { slot: r.uint(1), itemId: r.uint(4), count: r.uint(2), flags: r.uint(1) };
  if (e.flags & ~63) throw new Error('Unknown item record flags');
  OPTIONS.forEach((fields, i) => {
    if (e.flags & (1 << i)) for (const [key, size, type] of fields) e[key] = type === 'hex' ? r.take(size).toString('hex') : r.uint(size);
  });
  return e;
}
export function readItemRecord(bytes) {
  const r = new Reader(bytes);
  return { record: readRecord(r), bytesRead: r.offset };
}
export function recordOptions(e) {
  return OPTIONS.map((fields) => {
    const present = fields.map(([key]) => e[key] !== undefined);
    if (present.some(Boolean) && !present.every(Boolean)) throw new Error('Incomplete item record option');
    if (!present[0]) return null;
    return Buffer.concat(fields.map(([key, size, type]) => {
      if (type !== 'hex') return uintBytes(e[key], size, key);
      if (typeof e[key] !== 'string' || !new RegExp(`^[0-9a-fA-F]{${size * 2}}$`).test(e[key])) throw new Error(`Invalid ${key}`);
      return Buffer.from(e[key], 'hex');
    }));
  });
}
export function writeItemRecord(e) {
  const opts = recordOptions(e);
  const flags = opts.reduce((m, v, i) => m | (v ? 1 << i : 0), 0);
  return Buffer.concat([uintBytes(e.slot, 1, 'slot'), uintBytes(e.itemId, 4, 'item id'), uintBytes(e.count, 2, 'count'), Buffer.from([flags]), ...opts.filter(Boolean)]);
}
export function inventoryMask(c) {
  if (c.equipment?.length !== 16 || c.references?.length !== 4) throw new Error('Expected 16 equipment slots and 4 references');
  return c.equipment.reduce((m, e, i) => m | (e != null ? 1 << i : 0), 0)
    | c.references.reduce((m, ref, i) => m | (ref != null ? 1 << (i + 16) : 0), 0);
}
export function inventoryHeader(c) {
  integer(c.mainPages, 16, 'main pages'); integer(c.backpackPages, 16, 'backpack pages');
  integer(c.main.length + c.extra.length, 248, 'combined record count');
  return Buffer.from([c.mainPages, c.main.length, c.backpackPages, c.extra.length]);
}

export function readExactCharacter(bytes, version = 5) {
  integer(version, 5, 'character version');
  if (version < 3) throw new Error('Legacy character versions are read-only');
  const r = new Reader(bytes);
  r.take(version >= 5 ? 38 : 37); // gender, nine u32 fields, optional proportions flag
  r.take(20); // five stored attributes
  const level = r.uint(4), xp = r.uint(4), skills = [];
  const skillsCount = r.uint(4);
  if (skillsCount > Math.floor((r.bytes.length - r.offset) / 5)) throw new Error('Invalid skill count');
  for (let i = 0; i < skillsCount; i++) skills.push({ id: r.uint(4), level: r.uint(1) });
  const buffsCount = r.uint(4);
  if (buffsCount > Math.floor((r.bytes.length - r.offset) / 22)) throw new Error('Invalid buff count');
  for (let i = 0; i < buffsCount; i++) {
    r.take(20); // id:u32, two u64 values; in-memory stride is not the wire size
    const flags = r.uint(2);
    if (flags & ~63) throw new Error('Unknown buff record flags');
    [1, 4, 4, 1, 8, 52].forEach((n, bit) => { if (flags & (1 << bit)) r.take(n); });
  }
  const prefix = Buffer.from(r.bytes.subarray(0, r.offset));
  const mask = r.uint(4);
  if (mask & ~0xfffff) throw new Error('Unknown equipment mask bits');
  const equipment = Array.from({ length: 16 }, (_, i) => mask & (1 << i) ? readRecord(r) : null);
  const references = Array.from({ length: 4 }, (_, i) => mask & (1 << (16 + i)) ? r.uint(2) : null);
  const mainPages = r.uint(1), mainCount = r.uint(1), backpackPages = r.uint(1), extraCount = r.uint(1);
  if (mainPages > 16 || backpackPages > 16 || mainCount + extraCount > 248) throw new Error('Character inventory exceeds game limits');
  const main = Array.from({ length: mainCount }, () => readRecord(r));
  const extra = Array.from({ length: extraCount }, () => readRecord(r));
  const tail = Buffer.from(r.take(22)); // two actionbar page indices, selection, flag, four material ids
  if (r.offset !== r.bytes.length) throw new Error('Unknown trailing character data');
  return { version, level, xp, skills, equipment, references, mainPages, backpackPages, main, extra, _prefix: prefix, _tail: tail };
}

export function writeExactCharacter(c) {
  const header = inventoryHeader(c);
  const mask = inventoryMask(c);
  if (!Buffer.isBuffer(c._prefix) || !Buffer.isBuffer(c._tail) || c._tail.length !== 22) throw new Error('Missing exact character envelope');
  return Buffer.concat([c._prefix, uintBytes(mask, 4), ...c.equipment.filter((e) => e != null).map(writeItemRecord),
    ...c.references.filter((r) => r != null).map((r) => uintBytes(r, 2, 'reference')), header,
    ...c.main.map(writeItemRecord), ...c.extra.map(writeItemRecord), c._tail]);
}
