// SPDX-License-Identifier: MIT
// kiln.eml.startup v1 JSON records (schema/sfi/eml-startup/): the startup outcome kiln-eml returns and the native
// terminal kiln-sfi pushes. Schema contract tests, not tests of the GPL provider or the MIT adapter.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
const here = new URL('.', import.meta.url);
const text = p => readFileSync(new URL(p, here), 'utf8');
const read = p => JSON.parse(text(p));
// Load input first so the red test diagnoses missing contracts, not a dependency.
const valid = read('../sfi/eml-startup/fixtures/valid.json');
const invalid = read('../sfi/eml-startup/fixtures/invalid.json');
const outcomeSchema = read('../sfi/eml-startup/startup-outcome.schema.json');
const terminalSchema = read('../sfi/eml-startup/native-terminal.schema.json');
const commonDir = execFileSync('git', ['rev-parse', '--path-format=absolute', '--git-common-dir'],
  { cwd: fileURLToPath(here), encoding: 'utf8' }).trim();
const candidates = [process.env.AJV_2020,
  fileURLToPath(new URL('../../launcher/node_modules/ajv/dist/2020.js', here)),
  join(dirname(commonDir), 'launcher/node_modules/ajv/dist/2020.js')].filter(Boolean);
const found = candidates.find(existsSync);
assert(found, `AJV 2020 unavailable; tried ${candidates.join(', ')}`);
const Ajv = (await import(pathToFileURL(found).href)).default;
const ajv = new Ajv({ strict: false, validateFormats: false });
// One shared AJV with Kiln's top-level common and the other sfi schemas, as a combined sfi sweep would load them: the
// ids live under eml-startup/, so nothing shadows another schema.
const state = read('../sfi/sfi-state.schema.json');
const archive = read('../sfi/eml-archive/common.schema.json');
const kilnCommon = read('../common.schema.json');
ajv.addSchema(kilnCommon);
ajv.addSchema(archive);
ajv.addSchema(state);
let assertions = 0;
const files = readdirSync(new URL('../sfi/eml-startup/', here)).filter(n => n.endsWith('.schema.json')).sort();
assert.deepEqual(files, ['native-terminal.schema.json', 'startup-outcome.schema.json'], 'one schema per record'); ++assertions;
for (const f of files) {
  const schema = read(`../sfi/eml-startup/${f}`);
  assert.equal(schema.$id, `eml-startup/${f}`, `${f}: $id`); ++assertions;
  ajv.addSchema(schema);
}
const validator = name => ajv.getSchema(`eml-startup/${name}.schema.json`);

// The format number is the header's (kiln_eml_startup.h) and both records carry it.
const header = text('../../native/core/abi/services/kiln_eml_startup.h');
const format = Number(/#define KILN_EML_STARTUP_FORMAT (\d+)u/.exec(header)?.[1]);
assert.equal(format, 1, 'KILN_EML_STARTUP_FORMAT'); ++assertions;
for (const s of [outcomeSchema, terminalSchema]) { assert.equal(s.properties.format.const, format); ++assertions; }

// Shared grammar copied from the schemas it must agree with (relative $ref across directories is fragile in AJV).
const pin = archive.$defs.pin.properties;
for (const [ours, theirs, why] of [
  [outcomeSchema.$defs.sfId.pattern, state.$defs.sfId.pattern, 'sfId'],
  [terminalSchema.$defs.sfId.pattern, state.$defs.sfId.pattern, 'sfId (terminal)'],
  [outcomeSchema.$defs.code.pattern, state.$defs.code.pattern, 'code'],
  [terminalSchema.$defs.code.pattern, state.$defs.code.pattern, 'code (terminal)'],
  [outcomeSchema.properties.launchId.pattern, state.$defs.launchId.pattern, 'launchId'],
  [outcomeSchema.$defs.sha256.pattern, kilnCommon.$defs.sha256.pattern, 'sha256'],
  [outcomeSchema.$defs.uuid.pattern, pin.plan_id.pattern, 'planId'],
  [terminalSchema.$defs.uuid.pattern, pin.plan_id.pattern, 'planId (terminal)']]) {
  assert.equal(ours, theirs, why); ++assertions;
}
// Row data values are sfi-state's package data values, and committed / cached-verified mean present there too.
const stateData = state.$defs.package.properties.data.enum;
for (const d of outcomeSchema.$defs.row.properties.data.enum) { assert(stateData.includes(d), `data ${d}`); ++assertions; }
// found by what it says, not by its place: other rules (e.g. the ownerEpoch rule) come before it
const presentRule = state.$defs.package.allOf.find(r => r.if?.properties?.data?.enum);
assert(presentRule, 'sfi-state: the rule that data committed / cached-verified means present'); ++assertions;
assert.deepEqual(presentRule.if.properties.data.enum, ['committed', 'cached-verified']); ++assertions;
assert.equal(presentRule.then.properties.archivePresent.const, true); ++assertions;
// The base-origin tokens and labels are kiln-eml's frozen ORIGINS (inspection.rs), unrecorded is null.
const inspection = text('../../gpl/kiln-eml/kiln-eml/src/inspection.rs');
const constant = name => {
  const m = new RegExp(`pub const ${name}: &str =\\s*"([^"]*)";`).exec(inspection);
  assert(m, `inspection.rs ${name}`); return m[1];
};
const origins = outcomeSchema.properties.baseOrigin.oneOf.filter(o => o.type === 'object')
  .map(o => [o.properties.token.const, o.properties.label.const]);
assert.deepEqual(origins, [[constant('FIRST_MANAGED'), constant('FIRST_MANAGED_LABEL')],
                           [constant('REBASELINED'), constant('REBASELINED_LABEL')]]); ++assertions;
assert.equal(constant('UNRECORDED'), 'unrecorded'); ++assertions;

// Bounds: lists that mirror the plan (terminal dlls, outcome rows, withdrawn packages) and ids are never clamped, so
// they hold whatever the plan reader (sfi-policy launch_plan.rs) accepts; the outcome rows are sfi-state's rows.
const planReader = text('../../crates/sfi/sfi-policy/src/launch_plan.rs');
const planBound = re => { const m = re.exec(planReader); assert(m, `launch_plan.rs ${re}`); return Number(m[1]); };
const maxPackages = planBound(/packages\.len\(\) > (\d+)/);
const maxRecords = planBound(/total_records > (\d+)/);
const maxPath = planBound(/s\.len\(\) > (\d+)/);
assert.equal(outcomeSchema.properties.packages.maxItems, maxPackages, 'outcome rows: the plan bound'); ++assertions;
assert.equal(outcomeSchema.properties.packages.maxItems, state.properties.packages.maxItems, 'outcome rows: sfi-state'); ++assertions;
assert.equal(terminalSchema.properties.withdrawn.maxItems, maxPackages, 'withdrawn: the plan bound'); ++assertions;
assert.equal(terminalSchema.properties.dlls.maxItems, maxRecords, 'terminal dlls: the plan record budget'); ++assertions;
assert(terminalSchema.properties.dlls.items.properties.statusId.maxLength >= 'dll:'.length + 2 * maxPath, 'statusId: two plan paths'); ++assertions;
for (const [ours, why] of [[outcomeSchema.$defs.row.properties.chain.maxItems, 'row chain'],
  [terminalSchema.properties.withdrawn.items.properties.chain.maxItems, 'withdrawn chain']]) {
  assert.equal(ours, state.$defs.skip.properties.chain.maxItems, `${why}: sfi-state's chain bound`); ++assertions;
}

// Field order: a typed serializer writes these, in this order (fixtures show it).
const ORDER = {
  'startup-outcome': ['format', 'launchId', 'planId', 'planSha256', 'dialect', 'state', 'grant', 'receiptSha256',
    'archiveSha256', 'baseOrigin', 'native', 'attempt', 'error', 'retryError', 'packages', 'warnings'],
  'native-terminal': ['format', 'planId', 'notArmed', 'deadlinePassed', 'coherent', 'withdrawn', 'dlls'],
};
const ROW = ['id', 'version', 'pass', 'data', 'archivePresent', 'code', 'text', 'chain'];
const inOrder = (keys, order) => assert.deepEqual(keys, order.filter(k => keys.includes(k)));
for (const [name, order] of Object.entries(ORDER)) {
  const schema = name === 'startup-outcome' ? outcomeSchema : terminalSchema;
  assert.deepEqual(Object.keys(schema.properties), order, `${name}: schema property order`); ++assertions;
  assert.deepEqual([...schema.required].sort(), [...order].sort(), `${name}: every field is required`); ++assertions;
}
assert.deepEqual(Object.keys(outcomeSchema.$defs.row.properties), ROW); ++assertions;

// Every valid fixture validates, in field order; format 2 and an unknown member at any object level are refused.
assert.deepEqual(Object.keys(valid).sort(), ['native-terminal', 'startup-outcome']); ++assertions;
for (const [name, records] of Object.entries(valid)) {
  const validate = validator(name);
  for (const [label, record] of Object.entries(records)) {
    assert(validate(record), `${name} ${label}: ${JSON.stringify(validate.errors)}`); ++assertions;
    inOrder(Object.keys(record), ORDER[name]); assert.equal(Object.keys(record).length, ORDER[name].length); ++assertions;
    const bad = structuredClone(record); bad.format = 2;
    assert.equal(validate(bad), false, `${name} ${label}: format`); ++assertions;
    const objects = [];
    const walk = (v, path) => { if (v && typeof v === 'object') { if (!Array.isArray(v)) objects.push(path);
      for (const [k, c] of Object.entries(v)) walk(c, [...path, k]); } };
    walk(record, []);
    for (const path of objects) {
      const nested = structuredClone(record);
      path.reduce((o, k) => o[k], nested).silent_extension = true;
      assert.equal(validate(nested), false, `${name} ${label}: unknown member at ${path.join('.') || '(root)'}`); ++assertions;
    }
  }
}
// Every invalid fixture (one valid fixture with one rule broken) is refused. "One rule broken" is checked too: each
// record is at most two leaf paths away from a valid fixture of its schema, so it cannot fail for a second reason.
assert(invalid.length >= 50);
const leaves = (a, b, path = '') => {
  if (JSON.stringify(a) === JSON.stringify(b)) return [];
  if (a && b && typeof a === 'object' && typeof b === 'object' && Array.isArray(a) === Array.isArray(b)) {
    const out = [...new Set([...Object.keys(a), ...Object.keys(b)])].flatMap(k => leaves(a[k], b[k], `${path}/${k}`));
    return out.length ? out : [`${path} (order)`];
  }
  return [path];
};
for (const { schema, why, record } of invalid) {
  const nearest = Math.min(...Object.values(valid[schema]).map(v => leaves(v, record).length));
  assert(nearest >= 1 && nearest <= 2, `${schema}: ${why}: ${nearest} changes from the nearest valid fixture`); ++assertions;
}
for (const { schema, why, record } of invalid) {
  const validate = validator(schema);
  assert(validate, schema);
  assert.equal(validate(record), false, `${schema}: ${why}`); ++assertions;
}

// Rules JSON Schema cannot say, held by the fixtures (kiln-eml writes them, kiln-sfi relies on them).
const outcomes = Object.values(valid['startup-outcome']);
for (const o of outcomes) {
  for (const r of o.packages) inOrder(Object.keys(r), ROW);
  const ids = o.packages.map(r => r.id);
  assert.equal(new Set(ids).size, ids.length, 'one row per package'); ++assertions;
  for (const r of o.packages) {
    // A chain never names its own row (empty for the package's own fault), as sfi-state's skip chain.
    assert(!r.chain.includes(r.id), `${r.id}: its own chain names it`); ++assertions;
  }
  for (const r of o.packages.filter(r => r.code === 'dependency-skipped')) {
    // Nearest first, as sfi-state's skip chain and the terminal's: the failed package comes last.
    assert.equal(o.packages.find(p => p.id === r.chain.at(-1))?.data, 'failed', 'the failed package comes last'); ++assertions;
  }
  if (o.attempt === 'retry') { assert(o.packages.some(r => r.data === 'failed' || r.code === 'startup-retry-failed')); ++assertions; }
}
// Coverage: every state, dialect, attempt and row data value has a fixture, and the export-only package is committed
// and present (the vacuous true sfi-state's committed rule needs).
for (const [field, values] of [['state', outcomeSchema.properties.state.enum], ['dialect', outcomeSchema.properties.dialect.enum],
                               ['attempt', outcomeSchema.properties.attempt.enum]]) {
  for (const v of values) { assert(outcomes.some(o => o[field] === v), `a fixture with ${field} ${v}`); ++assertions; }
}
for (const d of outcomeSchema.$defs.row.properties.data.enum) {
  assert(outcomes.some(o => o.packages.some(r => r.data === d)), `a row with data ${d}`); ++assertions;
}
const exportOnly = valid['startup-outcome'].committed_after_retry.packages.find(r => r.id === 'export-only');
assert.deepEqual([exportOnly.data, exportOnly.archivePresent], ['committed', true]); ++assertions;
for (const t of Object.values(valid['native-terminal'])) {
  const withdrawn = new Set(t.withdrawn.map(w => w.id));
  assert.equal(withdrawn.size, t.withdrawn.length, 'one withdrawn entry per package'); ++assertions;
  for (const d of t.dlls) {
    // One way only: a package whose first DLL loaded and whose second faulted is withdrawn, the first stays loaded.
    const admitted = d.outcome === 'loaded' || d.outcome === 'foreign-owned';
    if (!admitted) { assert(withdrawn.has(d.modId), `${d.statusId}: a DLL that did not load withdraws its package`); ++assertions; }
  }
  for (const w of t.withdrawn) { assert(!w.chain.includes(w.id), `${w.id}: its own chain names it`); ++assertions; }
  assert.equal(new Set(t.dlls.map(d => d.statusId)).size, t.dlls.length, 'one record per planned DLL'); ++assertions;
}
for (const v of terminalSchema.properties.dlls.items.properties.outcome.enum) {
  assert(Object.values(valid['native-terminal']).some(t => t.dlls.some(d => d.outcome === v)), `a DLL ${v}`); ++assertions;
}
console.log(`eml startup schemas and fixtures: ${assertions} assertions passed (not the provider or the adapter)`);
