// SPDX-License-Identifier: MIT
// kiln.eml.values v1 packet schemas and fixtures (schema/sfi/eml-values/). Schema contract tests, not tests of the GPL
// graph: the GPL test m6_value_graph.rs checks that the provider writes these fixtures.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
const here = new URL('.', import.meta.url);
const read = p => JSON.parse(readFileSync(new URL(p, here), 'utf8'));
// Load input first so the red test diagnoses missing contracts, not a dependency.
const packets = read('../sfi/eml-values/fixtures/packets.json');
const invalid = read('../sfi/eml-values/fixtures/invalid.json');
const raw = read('../sfi/eml-values/fixtures/raw.json');
const common = read('../sfi/eml-values/common.schema.json');
const archive = read('../sfi/eml-archive/common.schema.json');
const commonDir = execFileSync('git', ['rev-parse', '--path-format=absolute', '--git-common-dir'],
  { cwd: fileURLToPath(here), encoding: 'utf8' }).trim();
const candidates = [process.env.AJV_2020,
  fileURLToPath(new URL('../../launcher/node_modules/ajv/dist/2020.js', here)),
  join(dirname(commonDir), 'launcher/node_modules/ajv/dist/2020.js')].filter(Boolean);
const found = candidates.find(existsSync);
assert(found, `AJV 2020 unavailable; tried ${candidates.join(', ')}`);
const Ajv = (await import(pathToFileURL(found).href)).default;
const ajv = new Ajv({ strict: false, validateFormats: false });
// One shared AJV with Kiln's top-level and the archive's commons, as a combined sfi sweep would load them: the ids live
// under eml-values/, so no common shadows another, and every relative $ref resolves inside eml-values/.
ajv.addSchema(read('../common.schema.json'));
ajv.addSchema(archive);
ajv.addSchema(common);
let assertions = 0;
const schemaFiles = readdirSync(new URL('../sfi/eml-values/', here)).filter(n => n.endsWith('.schema.json'));
for (const f of schemaFiles) {
  const schema = read(`../sfi/eml-values/${f}`);
  assert.equal(schema.$id, `eml-values/${f}`, `${f}: $id`); ++assertions;
  if (f !== 'common.schema.json') ajv.addSchema(schema);
}
assert.notEqual(ajv.getSchema('eml-archive/common.schema.json').schema, ajv.getSchema('eml-values/common.schema.json').schema);
const schemaOf = { view: 'view', length: 'length', dirty: 'dirty', iter_next: 'iter-next', error: 'error' };
const validator = packet => ajv.getSchema(`eml-values/${schemaOf[packet]}.schema.json`);
assert.deepEqual(Object.values(schemaOf).map(s => `${s}.schema.json`).sort(),
  schemaFiles.filter(f => f !== 'common.schema.json').sort(), 'one schema per JSON packet'); ++assertions;

// RC-5: the scalar patterns are the archive schema's, copied (relative $id resolution across directories is fragile in AJV).
const archiveTag = tag => archive.$defs.value.oneOf.find(v => v.properties.tag.const === tag).properties.value.pattern;
for (const [ours, theirs] of [['u64', archive.$defs.u64.pattern], ['i64', archive.$defs.i64.pattern],
                              ['bytes', archive.$defs.bytes.pattern], ['f64', archiveTag('f64')],
                              ['string', archiveTag('string')]]) {
  assert.equal(common.$defs[ours].pattern, theirs, `${ours} pattern equals the archive schema's`); ++assertions;
}
const luaValue = ajv.compile({ $ref: 'eml-values/common.schema.json#/$defs/lua_value' });
const archiveValue = ajv.compile({ $ref: 'eml-archive/common.schema.json#/$defs/value' });
for (const tag of ['nil', 'bool', 'i64', 'f64', 'string']) {
  assert(archive.$defs.value.oneOf.some(v => v.properties.tag.const === tag), `the archive schema has tag ${tag}`); ++assertions;
}
// every Lua scalar the packets carry is also a valid value of the archive schema (the restriction, not a new grammar)
const scalars = [];
const collect = v => { if (v && typeof v === 'object') { if (['nil', 'bool', 'i64', 'f64', 'string'].includes(v.tag) &&
  !('class' in v)) scalars.push(v); Object.values(v).forEach(collect); } };
Object.values(packets).forEach(collect);
assert(scalars.length >= 10);
for (const v of scalars) {
  assert(luaValue(v), JSON.stringify(luaValue.errors)); ++assertions;
  assert(archiveValue(v), JSON.stringify(archiveValue.errors)); ++assertions;
}
// i64 is two's complement (the archive schema: kiln_eml_archive.h, docs eml-archive.md, its i64-min fixture)
const i64 = s => BigInt.asIntN(64, BigInt('0x' + s.slice(4)));
assert.equal(i64(packets.view_i64_negative.value.value), -1n); ++assertions;
assert.equal(i64(packets.view_i64_min.value.value), -(2n ** 63n)); ++assertions;
assert.equal(i64(read('../sfi/eml-archive/fixtures/values.json').find(v => v.tag === 'i64').value), -(2n ** 63n)); ++assertions;
assert(luaValue({ tag: 'i64', value: 'i64:8000000000000000' })); ++assertions;

// every fixture validates; schema_version 2 and an unknown member anywhere are rejected (closed objects, RC-12)
for (const [name, packet] of Object.entries(packets)) {
  const validate = validator(packet.packet);
  assert(validate, `${name}: packet ${packet.packet}`);
  assert(validate(packet), `${name}: ${JSON.stringify(validate.errors)}`); ++assertions;
  const badVersion = structuredClone(packet); badVersion.schema_version = 2;
  assert.equal(validate(badVersion), false, `${name}: version`); ++assertions;
  const extra = structuredClone(packet); extra.silent_extension = true;
  assert.equal(validate(extra), false, `${name}: unknown member`); ++assertions;
  const objects = [];
  const walk = (v, path) => { if (v && typeof v === 'object' && !Array.isArray(v)) { if (path.length) objects.push(path);
    for (const [k, c] of Object.entries(v)) walk(c, [...path, k]); } };
  walk(packet, []);
  for (const path of objects) {
    const nested = structuredClone(packet);
    path.reduce((o, k) => o[k], nested).silent_extension = true;
    assert.equal(validate(nested), false, `${name}: unknown member at ${path.join('.')}`); ++assertions;
  }
}
for (const { schema, why, packet } of invalid) {
  const validate = validator(schema);
  assert.equal(validate(packet), false, `${schema}: ${why}`); ++assertions;
}

// coverage: every class and every validation/conversion kind has a fixture; internal only at the root
const causes = [];
const levels = (e, depth) => { causes.push({ ...e, depth }); if (e.cause) levels(e.cause, depth + 1); };
Object.values(packets).filter(p => p.packet === 'error').forEach(p => levels(p.error, 1));
for (const cls of ['runtime', 'position_out_of_bounds', 'static_array', 'invalid_property', 'borrow', 'validation',
                   'conversion', 'lua', 'internal']) {
  assert(causes.some(c => c.class === cls), `a fixture of class ${cls}`); ++assertions;
}
for (const kind of ['incompatible_type', 'missing_field', 'invalid_type', 'invalid_guid', 'lua', 'mapping']) {
  assert(causes.some(c => c.class === 'validation' && c.kind === kind), `validation ${kind}`); ++assertions;
}
for (const kind of ['incompatible_type', 'invalid_guid', 'lua', 'mapping']) {
  assert(causes.some(c => c.class === 'conversion' && c.kind === kind), `conversion ${kind}`); ++assertions;
}
const classes = new Set(common.$defs.cause.oneOf.map(c => c.properties.class.const));
assert.deepEqual([...classes].sort(), ['borrow', 'conversion', 'lua', 'position_out_of_bounds', 'runtime', 'static_array',
  'invalid_property', 'validation'].sort(), 'closed class list (internal is root-only)'); ++assertions;
assert(causes.filter(c => c.class === 'internal').every(c => c.depth === 1)); ++assertions;
assert(causes.some(c => c.depth === 2 && c.class === 'lua' && c.host_error), 'validation -> lua -> host error'); ++assertions;
assert(causes.some(c => c.depth === 2 && c.class === 'borrow'), 'a borrow error nested under validation'); ++assertions;
assert(causes.some(c => c.found_userdata?.lua_kind === 'typed_value' && c.found_userdata.value_kind)); ++assertions;
assert(Object.values(packets).some(p => p.packet === 'iter_next' && p.key.tag !== 'nil' && p.value.tag === 'nil'),
  'a (k, nil) ITER_NEXT is not the end'); ++assertions;
// messages decode: non-UTF-8 and embedded NUL survive (bytes:), names stay plain strings
const hex = s => Buffer.from(s.slice(s.indexOf(':') + 1), 'hex');
assert(hex(packets.error_validation_incompatible_non_utf8.error.found).equals(Buffer.from([0xff, 0]))); ++assertions;
assert(hex(packets.error_invalid_property.error.key).includes(0)); ++assertions;
assert(hex(packets.view_string_bytes.value.value).equals(Buffer.from([0x61, 0, 0x62, 0xff]))); ++assertions;
for (const p of Object.values(packets).filter(p => p.packet === 'error')) {
  for (let c = p.error; c; c = c.cause) {
    assert(c.message.startsWith('bytes:')); ++assertions;
    if (c.class === 'validation' || c.class === 'conversion') {
      assert(hex(c.message).toString('latin1').startsWith(`at ${c.path}: `), 'a check level names its path'); ++assertions;
    }
  }
}

// raw packets (STRINGIFY, SOURCE_BYTES, export) have no envelope: None (payload 0) differs from Some(empty)
assert.deepEqual(Object.keys(raw).sort(), ['$comment', 'cases']);
const rawNames = new Set();
for (const c of raw.cases) {
  assert.deepEqual(Object.keys(c).sort(), ['name', 'payload', 'query']); ++assertions;
  assert(['STRINGIFY', 'SOURCE_BYTES', 'EXPORT'].includes(c.query)); ++assertions;
  assert(c.payload === null || /^(?:[0-9a-f]{2})*$/.test(c.payload)); ++assertions;
  rawNames.add(c.name);
}
const none = raw.cases.find(c => c.name === 'source_bytes_none'), empty = raw.cases.find(c => c.name === 'source_bytes_empty');
assert(none.payload === null && empty.payload === '', 'SOURCE_BYTES None versus Some(empty)'); ++assertions;
assert(raw.cases.some(c => c.query === 'STRINGIFY' && Buffer.from(c.payload, 'hex').includes(0)), 'embedded NUL'); ++assertions;
console.log(`eml values schemas and fixtures: ${assertions} assertions passed (not provider parity)`);
