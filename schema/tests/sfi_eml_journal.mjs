// SPDX-License-Identifier: MIT
// kiln-eml's startup change journal (schema/sfi/eml-journal/): the per-package record of what a kiln-mode ShroudForge
// pass changed, bound to one generation. Schema contract tests over the fixtures and, with EML_JOURNAL_DIR=<folder>,
// over journals kiln-eml's own tests wrote there (EML_JOURNAL_OUT). Not a test of the GPL writer itself.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
const here = new URL('.', import.meta.url);
const text = p => readFileSync(new URL(p, here), 'utf8');
const read = p => JSON.parse(text(p));
// Load input first so the red test diagnoses missing contracts, not a dependency.
const valid = read('../sfi/eml-journal/fixtures/valid.json');
const invalid = read('../sfi/eml-journal/fixtures/invalid.json');
const schema = read('../sfi/eml-journal/startup-journal.schema.json');
const commonDir = execFileSync('git', ['rev-parse', '--path-format=absolute', '--git-common-dir'],
  { cwd: fileURLToPath(here), encoding: 'utf8' }).trim();
const candidates = [process.env.AJV_2020,
  fileURLToPath(new URL('../../launcher/node_modules/ajv/dist/2020.js', here)),
  join(dirname(commonDir), 'launcher/node_modules/ajv/dist/2020.js')].filter(Boolean);
const found = candidates.find(existsSync);
assert(found, `AJV 2020 unavailable; tried ${candidates.join(', ')}`);
const Ajv = (await import(pathToFileURL(found).href)).default;
const ajv = new Ajv({ strict: false, validateFormats: false });
// The other sfi schemas in the same AJV, as a combined sweep loads them: the id lives under eml-journal/.
const outcome = read('../sfi/eml-startup/startup-outcome.schema.json');
const state = read('../sfi/sfi-state.schema.json');
const kilnCommon = read('../common.schema.json');
ajv.addSchema(kilnCommon);
ajv.addSchema(state);
ajv.addSchema(outcome);
let assertions = 0;
const files = readdirSync(new URL('../sfi/eml-journal/', here)).filter(n => n.endsWith('.schema.json')).sort();
assert.deepEqual(files, ['startup-journal.schema.json'], 'one schema'); ++assertions;
assert.equal(schema.$id, 'eml-journal/startup-journal.schema.json'); ++assertions;
ajv.addSchema(schema);
const validate = ajv.getSchema('eml-journal/startup-journal.schema.json');

// Kept out of the strictly read outcome: the outcome schema has no journal member and its format stays 1.
assert(!Object.keys(outcome.properties).some(k => /journal/i.test(k)), 'the outcome stays as it is'); ++assertions;
assert.equal(outcome.properties.format.const, 1); ++assertions;
// Shared grammar copied from the schemas it agrees with.
for (const [ours, theirs, why] of [
  [schema.$defs.sfId.pattern, state.$defs.sfId.pattern, 'sfId'],
  [schema.$defs.sha256.pattern, kilnCommon.$defs.sha256.pattern, 'sha256'],
  [schema.$defs.package.properties.version.maxLength, outcome.$defs.row.properties.version.maxLength, 'version'],
  [schema.properties.attempt.enum.join(), outcome.properties.attempt.enum.filter(a => a !== 'none').join(), 'attempt']]) {
  assert.equal(ours, theirs, why); ++assertions;
}
// Bounds: packages mirror the plan (the outcome's rows bound); each bucket's lists hold at most the writer's
// PACKAGE_ENTRIES, and the description names TOTAL_ENTRIES (kiln-eml sf_startup/journal.rs).
const writer = text('../../gpl/kiln-eml/kiln-eml/src/sf_startup/journal.rs');
const constant = name => { const m = new RegExp(`pub const ${name}: usize = (\\d+);`).exec(writer); assert(m, name); return Number(m[1]); };
const perPackage = constant('PACKAGE_ENTRIES');
const total = constant('TOTAL_ENTRIES');
assert.equal(schema.properties.packages.maxItems, outcome.properties.packages.maxItems, 'packages: the plan bound'); ++assertions;
for (const list of ['changedList', 'createdList', 'contentList']) {
  assert.equal(schema.$defs[list].maxItems, perPackage, list); ++assertions;
}
assert(schema.description.includes(`at most ${perPackage} entries`) && schema.description.includes(`${total} in all`)); ++assertions;

// Field order: a typed serializer writes these, in this order.
const ORDER = ['format', 'stem', 'generation', 'receiptSha256', 'dialect', 'attempt', 'truncated', 'packages', 'unattributed'];
const PACKAGE = ['id', 'version', 'kind', 'contentSha256', 'settingsSha256', 'truncated', 'changed', 'created', 'contents'];
const CHANGES = ['truncated', 'changed', 'created', 'contents'];
const CHANGED = ['guid', 'typeHash', 'part', 'createdBy'];
const CREATED = ['ordinal', 'guid', 'typeHash', 'part', 'explicit'];
const CONTENT = ['guid', 'size'];
assert.deepEqual(Object.keys(schema.properties), ORDER); ++assertions;
assert.deepEqual([...schema.required].sort(), [...ORDER].sort()); ++assertions;
for (const [def, order] of [['package', PACKAGE], ['changes', CHANGES]]) {
  assert.deepEqual(Object.keys(schema.$defs[def].properties), order, def); ++assertions;
  assert.deepEqual([...schema.$defs[def].required].sort(), [...order].sort(), def); ++assertions;
}
for (const [def, order] of [['changedList', CHANGED], ['createdList', CREATED], ['contentList', CONTENT]]) {
  assert.deepEqual(Object.keys(schema.$defs[def].items.properties), order, def); ++assertions;
  assert.deepEqual([...schema.$defs[def].items.required].sort(), [...order].sort(), def); ++assertions;
}

// Rules JSON Schema cannot say (the writer keeps them; consumers rely on them).
const keyOf = e => `${e.guid}/${e.typeHash}/${e.part}`;
function rules(j, label) {
  const inOrder = (o, order, where) => assert.deepEqual(Object.keys(o), order, `${label}: ${where} field order`);
  inOrder(j, ORDER, 'record');
  const ids = j.packages.map(p => p.id);
  assert.equal(new Set(ids).size, ids.length, `${label}: one entry per package`); ++assertions;
  const buckets = [...j.packages.map(p => [p.id, p]), [null, j.unattributed]];
  const created = new Map(); // key -> [package, ordinal]
  let entries = 0;
  for (const [id, b] of buckets) {
    inOrder(b, id === null ? CHANGES : PACKAGE, id ?? 'unattributed');
    for (const e of b.changed) inOrder(e, CHANGED, 'changed');
    for (const e of b.created) inOrder(e, CREATED, 'created');
    for (const e of b.contents) inOrder(e, CONTENT, 'contents');
    const n = b.changed.length + b.created.length + b.contents.length;
    assert(n <= perPackage, `${label}: ${id}: ${n} entries`); ++assertions;
    entries += n;
    // ordinals 1, 2, ... in creation order (the ones over the bound are the last)
    b.created.forEach((e, i) => assert.equal(e.ordinal, i + 1, `${label}: ${id}: ordinals`)); ++assertions;
    for (const e of b.created) {
      assert(!created.has(keyOf(e)), `${label}: ${keyOf(e)} created twice`); ++assertions;
      created.set(keyOf(e), [id, e.ordinal]);
    }
    const changedKeys = b.changed.map(keyOf);
    assert.equal(new Set(changedKeys).size, changedKeys.length, `${label}: ${id}: a resource listed twice`); ++assertions;
    const contentKeys = b.contents.map(c => c.guid);
    assert.equal(new Set(contentKeys).size, contentKeys.length, `${label}: ${id}: a content listed twice`); ++assertions;
    // a content's GUID begins with its size (the first group is the size, little-endian)
    for (const c of b.contents) { assert.equal(parseInt(c.guid.slice(0, 8), 16), c.size, `${label}: ${c.guid}`); ++assertions; }
  }
  assert(entries <= total, `${label}: ${entries} entries in all`); ++assertions;
  assert.equal(j.truncated, buckets.some(([, b]) => b.truncated), `${label}: truncated exactly when a bucket is`); ++assertions;
  // createdBy names the package (never the writer itself) whose created list holds that ordinal under that key
  for (const [id, b] of buckets) {
    for (const e of b.changed) {
      if (e.createdBy === null) {
        assert(!created.has(keyOf(e)) || created.get(keyOf(e))[0] === null, `${label}: ${id}: a created resource without its creator`); ++assertions;
        continue;
      }
      assert.notEqual(e.createdBy.package, id, `${label}: ${id} lists its own creation as changed`); ++assertions;
      const creator = j.packages.find(p => p.id === e.createdBy.package);
      assert(creator, `${label}: ${id}: createdBy names no package of the journal`); ++assertions;
      const entry = creator.created.find(c => c.ordinal === e.createdBy.ordinal);
      if (entry) { assert.equal(keyOf(entry), keyOf(e), `${label}: createdBy's key`); ++assertions; }
      else { assert(creator.truncated, `${label}: createdBy names a creation its creator does not list`); ++assertions; }
      if (id !== null) {
        assert(ids.indexOf(e.createdBy.package) < ids.indexOf(id), `${label}: created after its writer ran`); ++assertions;
      }
    }
  }
}

// Every valid fixture validates and keeps the rules; format 2 and an unknown member at any object level are refused.
assert.deepEqual(Object.keys(valid), ['startup-journal']); ++assertions;
for (const [label, record] of Object.entries(valid['startup-journal'])) {
  assert(validate(record), `${label}: ${JSON.stringify(validate.errors)}`); ++assertions;
  rules(record, label);
  const bad = structuredClone(record); bad.format = 2;
  assert.equal(validate(bad), false, `${label}: format`); ++assertions;
  const objects = [];
  const walk = (v, path) => { if (v && typeof v === 'object') { if (!Array.isArray(v)) objects.push(path);
    for (const [k, c] of Object.entries(v)) walk(c, [...path, k]); } };
  walk(record, []);
  for (const path of objects) {
    const nested = structuredClone(record);
    path.reduce((o, k) => o[k], nested).silent_extension = true;
    assert.equal(validate(nested), false, `${label}: unknown member at ${path.join('.') || '(root)'}`); ++assertions;
  }
}
// Every invalid fixture (one valid fixture with one rule broken, at most two leaf paths away) is refused.
assert(invalid.length >= 40);
const leaves = (a, b, path = '') => {
  if (JSON.stringify(a) === JSON.stringify(b)) return [];
  if (a && b && typeof a === 'object' && typeof b === 'object' && Array.isArray(a) === Array.isArray(b)) {
    const out = [...new Set([...Object.keys(a), ...Object.keys(b)])].flatMap(k => leaves(a[k], b[k], `${path}/${k}`));
    return out.length ? out : [`${path} (order)`];
  }
  return [path];
};
for (const { schema: name, why, record } of invalid) {
  assert.equal(name, 'startup-journal');
  const nearest = Math.min(...Object.values(valid['startup-journal']).map(v => leaves(v, record).length));
  assert(nearest >= 1 && nearest <= 2, `${why}: ${nearest} changes from the nearest valid fixture`); ++assertions;
  assert.equal(validate(record), false, why); ++assertions;
}
// Coverage: both stems, both attempts, both kinds, a created resource named by another package, an explicit and a
// random creation, a truncated journal, an unattributed write.
const records = Object.values(valid['startup-journal']);
for (const [what, ok] of [
  ['stem enshrouded', records.some(r => r.stem === 'enshrouded')],
  ['stem enshrouded_server', records.some(r => r.stem === 'enshrouded_server')],
  ['attempt first', records.some(r => r.attempt === 'first')],
  ['attempt retry', records.some(r => r.attempt === 'retry')],
  ['kind eml', records.some(r => r.packages.some(p => p.kind === 'eml'))],
  ['createdBy', records.some(r => r.packages.some(p => p.changed.some(c => c.createdBy)))],
  ['explicit and random', records.some(r => r.packages.some(p => p.created.some(c => c.explicit) && p.created.some(c => !c.explicit)))],
  ['truncated', records.some(r => r.truncated)],
  ['unattributed', records.some(r => r.unattributed.changed.length > 0)]]) {
  assert(ok, `a fixture with ${what}`); ++assertions;
}
// The journals kiln-eml's tests wrote (EML_JOURNAL_OUT), when given: valid, in field order (raw text), the rules kept.
let written = 0;
if (process.env.EML_JOURNAL_DIR) {
  const dir = process.env.EML_JOURNAL_DIR;
  for (const name of readdirSync(dir).filter(n => n.endsWith('.json')).sort()) {
    const raw = readFileSync(join(dir, name), 'utf8');
    const record = JSON.parse(raw);
    assert(validate(record), `${name}: ${JSON.stringify(validate.errors)}`); ++assertions;
    rules(record, name);
    let at = 0;
    for (const field of ORDER) { const i = raw.indexOf(`"${field}":`, at); assert(i >= at, `${name}: ${field}`); at = i; }
    ++written;
  }
  assert(written > 0, `no journal in ${dir}`);
}
console.log(`eml startup journal schema and fixtures: ${assertions} assertions passed${written ? `, ${written} written journals` : ''} (not the writer)`);
