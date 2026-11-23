// SPDX-License-Identifier: MIT
// Player storage sidecars are Kiln data, never part of the game's character format.
import { Buffer } from 'buffer';
import { integer, readItemRecord } from './character-exact.mjs';
import { inventoryWitness } from './storage-witness.mjs';

const object = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);
const guid = (s) => typeof s === 'string' ? s.replaceAll('-', '').toLowerCase() : '';
const clone = (v) => JSON.parse(JSON.stringify(v));
const witness = (s) => typeof s === 'string' && /^xxh3:[0-9a-f]{16}$/.test(s);
const revision = (v) => integer(v, Number.MAX_SAFE_INTEGER - 1, 'storage revision');
function documentOf(sidecar, id) {
  if (!object(sidecar) || sidecar.format !== 1 || guid(sidecar.character) !== guid(id)) throw new Error('Storage belongs to another character or format');
  const doc = sidecar.mods?.['kiln-items']?.storage;
  if (!object(doc) || guid(doc.character) !== guid(id)) throw new Error('Storage belongs to another character');
  if (Buffer.byteLength(JSON.stringify(doc), 'utf8') > 65536) throw new Error('Storage document is too large');
  if (doc.v !== 1) throw new Error('This storage needs a newer Kiln version');
  revision(sidecar.revision); revision(doc.revision);
  if (!object(doc.current) || !Array.isArray(doc.history) || doc.history.length > 8) throw new Error('Invalid storage history');
  return doc;
}
function candidates(doc) {
  const out = [];
  for (const [index, entry] of [doc.current, ...doc.history].entries()) {
    if (!object(entry) || !witness(entry.witness)) throw new Error('Invalid storage witness');
    const rev = revision(index === 0 ? doc.revision : entry.revision);
    out.push({ entry, witness: entry.witness, revision: rev });
    if (entry.pairs !== undefined && (!Array.isArray(entry.pairs) || entry.pairs.length > 32)) throw new Error('Invalid storage pairs');
    for (const pair of entry.pairs ?? []) {
      if (!object(pair) || !witness(pair.witness)) throw new Error('Invalid storage pair');
      out.push({ entry, witness: pair.witness, revision: revision(pair.revision) });
    }
  }
  return out;
}
export function selectStoragePair(doc, actualWitness) {
  const all = candidates(doc);
  const paired = all.filter((p) => p.witness === actualWitness);
  return [...(paired.length ? paired : all)].sort((a, b) => b.revision - a.revision)[0];
}
function groupsOf(pair) {
  const groups = [];
  for (const field of ['storages', 'parked']) {
    if (!object(pair[field])) throw new Error('Invalid storage list');
    for (const [key, storage] of Object.entries(pair[field])) {
      if (!object(storage) || !Array.isArray(storage.records)) throw new Error('Invalid storage records');
      integer(storage.slots, 256, 'storage slots');
      if (!storage.slots || storage.records.length > storage.slots) throw new Error('Storage record limit exceeded');
      const slots = new Set();
      for (const record of storage.records) {
        if (!object(record)) throw new Error('Invalid storage record');
        integer(record.s, storage.slots - 1, 'storage slot');
        if (slots.has(record.s)) throw new Error('Duplicate storage slot');
        slots.add(record.s);
      }
      if (storage.refs !== undefined && !object(storage.refs)) throw new Error('Invalid storage references');
      groups.push({ key, field, storage });
    }
  }
  if (groups.length > 32) throw new Error('Too many player storages');
  return groups;
}
function decoded(row) {
  if (typeof row.b64 !== 'string' || row.b64.length > 64 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(row.b64)) throw new Error('Invalid record bytes');
  const b = Buffer.from(row.b64, 'base64');
  const { record, bytesRead } = readItemRecord(b);
  if (bytesRead !== b.length || record.slot !== row.s || !record.itemId || !record.count) throw new Error('Invalid record contents');
  return record;
}
export function storageHasServerCopy(doc) {
  return [doc?.current, ...(doc?.history ?? [])].some((pair) => pair?.source === 'server' || pair?.source?.kind === 'server');
}
export async function storageView(c, sidecar, id) {
  if (!sidecar?.mods?.['kiln-items']?.storage) return { hasStorage: false, readOnly: true, canReturn: false, groups: [] };
  try {
    const doc = documentOf(sidecar, id);
    const actual = await inventoryWitness(c);
    const chosen = selectStoragePair(doc, actual);
    let invalidRecords = 0;
    const groups = groupsOf(chosen.entry).map(({ key, field, storage }) => ({ key, parked: field === 'parked', slots: storage.slots,
      records: storage.records.flatMap((r) => {
        const row = { slot: r.s, itemId: r.item, count: r.n, level: r.level,
          durability: r.durability, maxDurability: r.maxDurability, liquid: r.liquid, maxLiquid: r.maxLiquid,
          appearance: r.appearance, maxUpgrades: r.maxUpgrades, upgrades: r.upgrades, socket: r.socket };
        const valid = Number.isInteger(row.itemId) && row.itemId > 0 && row.itemId <= 0xffffffff
          && Number.isInteger(row.count) && row.count >= 0 && row.count <= 65535
          && Object.entries(row).every(([k, v]) => v === undefined || (k === 'appearance' || k === 'socket'
            ? typeof v === 'string' && new RegExp(`^[a-fA-F0-9]{${k === 'appearance' ? 8 : 10}}$`).test(v)
            : Number.isInteger(v) && v >= 0 && v <= 0xffffffff));
        if (!valid) { invalidRecords++; return []; }
        return [row];
      }) }));
    return { hasStorage: true, readOnly: true, canReturn: true, groups, invalidRecords, paired: chosen.witness === actual, serverCopy: storageHasServerCopy(doc), quarantined: doc.quarantine?.length ?? 0 };
  } catch (e) { return { hasStorage: true, readOnly: true, canReturn: false, groups: [], error: e.message }; }
}

// A sidecar may be one atomic write ahead of the character. Always select using the on-disk
// character's witness; blindly reading current could lose stacks after an interrupted return.
export async function planStorageReturn(c, sidecar, id, maxStackOf) {
  const doc = documentOf(sidecar, id);
  const beforeWitness = await inventoryWitness(c);
  const chosen = selectStoragePair(doc, beforeWitness);
  const all = candidates(doc);
  const nextRevision = Math.max(...all.map((p) => p.revision)) + 1;
  revision(nextRevision);
  const copy = clone(sidecar), nextDoc = copy.mods['kiln-items'].storage;
  const next = clone(chosen.entry);
  delete next.revision; delete next.pairs;
  const groups = groupsOf(next);
  for (const [pages, records] of [[c.mainPages, c.main], [c.backpackPages, c.extra]]) {
    integer(pages, 16, 'pages');
    if (records.some((r) => !Number.isInteger(r.slot) || r.slot < 0 || r.slot >= pages * 8) || new Set(records.map((r) => r.slot)).size !== records.length) throw new Error('Character has invalid inventory slots');
  }
  const room = Math.min((16 - c.mainPages) * 8, 248 - c.main.length - c.extra.length);
  if (room < 0) throw new Error('Character inventory exceeds game limits');
  const returned = [], skipped = [];
  let modItems = 0;
  for (const { key, storage } of groups) {
    const keep = [];
    for (const row of storage.records) {
      try {
        const record = decoded(row);
        const max = maxStackOf(record.itemId);
        if (!Number.isInteger(max)) {
          keep.push(row); modItems++; skipped.push({ key, slot: row.s, reason: 'Item needs its mod' }); continue;
        }
        if (max < record.count) throw new Error('Item exceeds its stack limit');
        if (returned.length >= room) { keep.push(row); continue; }
        returned.push({ ...record, slot: c.mainPages * 8 + returned.length });
        for (const [ref, index] of Object.entries(storage.refs ?? {})) if (index === row.s) delete storage.refs[ref];
      } catch (e) { keep.push(row); skipped.push({ key, slot: row.s, reason: e.message }); }
    }
    storage.records = keep;
  }
  const remaining = groups.reduce((n, g) => n + g.storage.records.length, 0);
  if (!returned.length) return { character: c, sidecar, returned: 0, remaining, modItems, skipped };
  const character = { ...c, mainPages: c.mainPages + Math.ceil(returned.length / 8), main: [...c.main, ...returned] };
  next.witness = await inventoryWitness(character);
  nextDoc.revision = nextRevision; nextDoc.current = next;
  // First retain the FULL storage paired to the actual pre-write file, even if that file had
  // no previous match (vanilla play), or matched the oldest alias. This pair must never be trimmed.
  const historical = [{ ...clone(chosen.entry), witness: beforeWitness, revision: chosen.revision },
    { ...clone(doc.current), revision: doc.revision }, ...clone(doc.history)];
  const history = [], seen = new Set([next.witness]);
  let budget = 8; // eight historical pairs; aliases share this budget
  for (const entry of historical) {
    if (budget === 0) break;
    if (seen.has(entry.witness)) continue;
    seen.add(entry.witness); budget--;
    const pairs = [];
    for (const p of entry.pairs ?? []) if (budget > 0 && !seen.has(p.witness)) { pairs.push(p); seen.add(p.witness); budget--; }
    delete entry.pairs;
    if (pairs.length) entry.pairs = pairs;
    history.push(entry);
  }
  nextDoc.history = history;
  copy.revision = Math.max(sidecar.revision, nextRevision) + 1;
  revision(copy.revision);
  if (Buffer.byteLength(JSON.stringify(nextDoc), 'utf8') > 65536) throw new Error('Storage history is too large to return safely');
  return { character, sidecar: copy, returned: returned.length, remaining, modItems, skipped };
}
