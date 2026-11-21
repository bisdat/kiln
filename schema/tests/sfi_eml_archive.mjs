// SPDX-License-Identifier: MIT
// Schema and binary TRANSPORT contract tests, not tests of the GPL codec/provider.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
const here = new URL('.', import.meta.url);
const read = p => JSON.parse(readFileSync(new URL(p, here), 'utf8'));
// Load input first so the red test diagnoses missing contracts, not a dependency.
const fixtures = read('../sfi/eml-archive/fixtures/payloads.json');
const common = read('../sfi/eml-archive/common.schema.json');
const commonDir = execFileSync('git', ['rev-parse', '--path-format=absolute', '--git-common-dir'],
  { cwd: fileURLToPath(here), encoding: 'utf8' }).trim();
const candidates = [process.env.AJV_2020,
  fileURLToPath(new URL('../../launcher/node_modules/ajv/dist/2020.js', here)),
  join(dirname(commonDir), 'launcher/node_modules/ajv/dist/2020.js')].filter(Boolean);
const found = candidates.find(existsSync);
assert(found, `AJV 2020 unavailable; tried ${candidates.join(', ')}`);
const Ajv = (await import(pathToFileURL(found).href)).default;
const ajv = new Ajv({ strict: false, validateFormats: false });
// One shared AJV with Kiln's top-level common.schema.json, as a combined sfi sweep would load them: the eml-archive
// ids live under eml-archive/ so neither common shadows the other, and every relative $ref resolves inside eml-archive/.
ajv.addSchema(read('../common.schema.json'));
ajv.addSchema(common);
let assertions = 0;
for (const f of readdirSync(new URL('../sfi/eml-archive/', here)).filter(n => n.endsWith('.schema.json'))) {
  assert.equal(read(`../sfi/eml-archive/${f}`).$id, `eml-archive/${f}`, `${f}: $id`); ++assertions;
}
assert.notEqual(ajv.getSchema('common.schema.json').schema, ajv.getSchema('eml-archive/common.schema.json').schema);
for (const [name, payload] of Object.entries(fixtures)) {
  const validate = ajv.compile(read(`../sfi/eml-archive/${name}.schema.json`));
  assert(validate(payload), `${name}: ${JSON.stringify(validate.errors)}`); ++assertions;
  const badVersion = structuredClone(payload); badVersion.schema_version = 2;
  assert.equal(validate(badVersion), false, `${name}: version`); ++assertions;
  const extra = structuredClone(payload); extra.silent_extension = true;
  assert.equal(validate(extra), false, `${name}: unknown member`); ++assertions;
}
const registry = ajv.getSchema('eml-archive/registry.schema.json');
const metadataCases = read('../sfi/eml-archive/fixtures/metadata.json');
// FINAL game.version/game.types.version: both pins' game/{mod,types}.rs return
// the original strings; load_type_registry.rs:71-111 does not parse the tag.
for (const versions of metadataCases.registry_versions) {
  const d = { ...structuredClone(fixtures.registry), ...versions };
  assert(registry(d), `original version strings: ${JSON.stringify(registry.errors)}`); ++assertions;
  assert.deepEqual(JSON.parse(JSON.stringify(d)).game_version, versions.game_version); ++assertions;
  assert.deepEqual(JSON.parse(JSON.stringify(d)).registry_version, versions.registry_version); ++assertions;
}
for (const field of ['game_version', 'registry_version', 'count', 'first']) {
  const d = structuredClone(fixtures.registry); d[field] = 26430625;
  assert.equal(registry(d), false, `${field}: no numeric coercion`); ++assertions;
}
for (const field of ['count', 'first']) {
  const d = structuredClone(fixtures.registry); d[field] = '26430625';
  assert.equal(registry(d), false, `${field}: quantities retain u64 tags`); ++assertions;
}
for (const change of [d => d.pin.generation = 9007199254740993,
                      d => d.types[0].default_bytes = 0,
                      d => d.types[0].index = 1.5,
                      d => d.types[0].enum_fields[0].value = 18446744073709551615,
                      d => d.pin.launch_id = '00000000']) {
  const d = structuredClone(fixtures.registry); change(d);
  assert.equal(registry(d), false); ++assertions;
}
// Both pins' build_attribute_catalog.rs:427-442,522-550 keep unresolved scalar
// descriptors; read_attribute_{catalog,descriptor}.rs return them successfully.
for (const entry of metadataCases.unresolved_attributes) {
  for (const name of ['attribute-catalog', 'attribute-resolve']) {
    const d = structuredClone(fixtures[name]);
    if (name === 'attribute-catalog') d.entries.push(structuredClone(entry));
    else d.entry = structuredClone(entry);
    const validate = ajv.getSchema(`eml-archive/${name}.schema.json`);
    assert(validate(d), `${name}: unresolved scalar metadata: ${JSON.stringify(validate.errors)}`); ++assertions;
    const result = JSON.parse(JSON.stringify(d));
    const copied = name === 'attribute-catalog' ? result.entries.at(-1) : result.entry;
    assert.deepEqual(copied, entry); ++assertions;
    assert.equal(copied.scalar_type, null); ++assertions;
    const fields = Object.fromEntries(copied.descriptor.entries.map(({key, value}) =>
      [Buffer.from(key.value.slice('string:'.length), 'hex').toString('utf8'), value]));
    assert.equal(BigInt(fields.hash.value.replace('u64:', '0x')), BigInt(entry.id)); ++assertions;
    assert.equal(fields.scalar_type.tag, 'nil'); ++assertions;
    assert.equal(fields.storage_writable.value, false); ++assertions;
    assert.equal(fields.calculation_writable.value, false); ++assertions;
    assert.equal(fields.storage_reason.tag, 'string'); ++assertions;
    assert.equal(fields.calculation_reason.tag, 'string'); ++assertions;
    const invalid = structuredClone(d);
    const invalidEntry = name === 'attribute-catalog' ? invalid.entries.at(-1) : invalid.entry;
    invalidEntry.scalar_type = 42;
    assert.equal(validate(invalid), false, `${name}: scalar_type must remain string or null`); ++assertions;
    delete invalidEntry.scalar_type;
    assert.equal(validate(invalid), false, `${name}: scalar_type remains required`); ++assertions;
  }
}
const value = ajv.compile({ $ref: 'eml-archive/common.schema.json#/$defs/value' });
const tagged = read('../sfi/eml-archive/fixtures/values.json');
for (const v of tagged) { assert(value(v), JSON.stringify(value.errors)); ++assertions; }
for (const v of [{tag:'u64', value:18446744073709551615}, {tag:'f64', value:1.5},
                 {tag:'f32', value:'f32:7fc0000'}, {tag:'bytes', value:'bytes:0'},
                 {tag:'optional', present:false, value:{tag:'nil'}}, {tag:'unknown'}]) {
  assert.equal(value(v), false); ++assertions;
}

// Independent byte parser for the fixed neutral transport, deliberately not a kfc codec.
// BigInt arithmetic validates sizes before converting to native JS offsets.
function parseNode(bytes, begin = 0, bound = bytes.length) {
  assert(bound - begin >= 48);
  const u32 = n => bytes.readUInt32LE(begin+n);
  const u64 = n => bytes.readBigUInt64LE(begin+n);
  assert.equal(u32(0), 48); assert.equal(u32(4), 1);
  const tag = u32(8); assert(tag <= 13); assert.equal(u32(12), 0); assert.equal(u32(44), 0);
  const total = u64(16), payload = u64(24), children = u64(32), type = BigInt(u32(40));
  assert(total >= 48n + type + payload);
  assert(total <= BigInt(bound - begin));
  assert(children <= (total-48n-type-payload)/48n);
  const end = begin + Number(total), p = begin + 48 + Number(type);
  new TextDecoder('utf-8', {fatal:true}).decode(bytes.subarray(begin+48,p));
  assert(!bytes.subarray(begin+48,p).includes(0));
  const scalarSizes = new Map([[0,0n],[1,1n],[2,8n],[3,8n],[4,4n],[5,8n],[6,16n],[13,16n]]);
  if (scalarSizes.has(tag)) { assert.equal(payload, scalarSizes.get(tag)); assert.equal(children,0n); }
  if (tag === 1) assert(bytes[p] <= 1);
  if (tag === 7 || tag === 8) assert.equal(children,0n);
  if (tag === 9 || tag === 10) assert.equal(payload,0n);
  if (tag === 10) assert.equal(children%2n,0n);
  if (tag === 11) { assert.equal(payload,8n); assert.equal(children,1n); }
  if (tag === 12) { assert.equal(payload,1n); assert(bytes[p]<=1); assert.equal(children,BigInt(bytes[p])); }
  let next = p + Number(payload);
  for (let i = 0n; i < children; ++i) next = parseNode(bytes,next,end);
  assert.equal(next,end);
  return end;
}
const packets = read('../sfi/eml-archive/fixtures/trees.json');
for (const packet of packets) {
  const bytes = Buffer.from(packet.hex,'hex');
  assert.equal(parseNode(bytes),bytes.length,packet.name); ++assertions;
  for (const offset of [0,4,8,12,44]) {
    const bad = Buffer.from(bytes); bad.writeUInt32LE(0xffffffff,offset);
    assert.throws(() => parseNode(bad)); ++assertions;
  }
  const short = bytes.subarray(0,bytes.length-1);
  assert.throws(() => parseNode(short)); ++assertions;
  for (const offset of [16,24,32]) {
    const bad = Buffer.from(bytes); bad.writeBigUInt64LE(0xffffffffffffffffn,offset);
    assert.throws(() => parseNode(bad)); ++assertions;
  }
}
console.log(`eml archive schemas and binary transport: ${assertions} assertions passed (not provider parity)`);
