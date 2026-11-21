// SPDX-License-Identifier: MIT
// The three closed schemas in schema/sfi/ against their fixtures in schema/fixtures/sfi/,
// the shipped save-sensitive policy schema/sfi/sfi-safety-policy.json, and the reader rules a schema cannot say.
//   node schema/tests/sfi_review.mjs
// ajv is looked up as in closed_dll_schemas.mjs: $AJV_2020, this tree's launcher/node_modules, then the main checkout's.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = new URL(".", import.meta.url);
const read = (file) => JSON.parse(readFileSync(new URL(file, here), "utf8"));
const mainCheckout = () => {
  try {
    const common = execFileSync("git", ["rev-parse", "--path-format=absolute", "--git-common-dir"],
      { cwd: fileURLToPath(here), encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] }).trim();
    return join(dirname(common), "launcher/node_modules/ajv/dist/2020.js");
  } catch { return null; }
};
const candidates = [process.env.AJV_2020, fileURLToPath(new URL("../../launcher/node_modules/ajv/dist/2020.js", here)), mainCheckout()]
  .filter(Boolean);
const found = candidates.find((p) => existsSync(p));
assert(found, `ajv 2020 not found; set AJV_2020 (tried ${candidates.join(", ")})`);
const Ajv2020 = (await import(pathToFileURL(found).href)).default;
const ajv = new Ajv2020({ strict: false, validateFormats: false, allErrors: true });
ajv.addSchema(read("../common.schema.json"));
const schemas = {
  state: ajv.compile(read("../sfi/sfi-state.schema.json")),
  safety: ajv.compile(read("../sfi/sfi-safety-policy.schema.json")),
  ui: ajv.compile(read("../sfi/sfi-ui-envelope.schema.json")),
  // kiln-manage's activation journal (crates/kiln-manage/src/sfi_activation.rs; its test the_journal_has_the_shape_of_its_schema
  // checks a journal the activation writes against this schema)
  activation: ajv.compile(read("../sfi/sfi-activation.schema.json")),
};
const sha = (s) => createHash("sha256").update(s, "utf8").digest("hex");
const errs = (v) => JSON.stringify(v.errors?.map((e) => `${e.instancePath} ${e.message}`));

// Private: the sfi schemas live only in schema/sfi/, so the public sweeps (tools/vscode sync, launcher check:schemas,
// kiln lint's schemas.json) that read schema/*.schema.json never pick them up.
const top = readdirSync(new URL("../", here));
assert.deepEqual(top.filter((n) => /^sfi[-_]/.test(n)), [], "no sfi schema at the top of schema/");
assert(!readFileSync(new URL("../schemas.json", here), "utf8").includes("sfi-"), "schemas.json maps no sfi schema");
for (const f of readdirSync(new URL("../sfi/", here)).filter((n) => n.endsWith(".schema.json"))) {
  const s = read(`../sfi/${f}`);
  assert.equal(s.$id, f, `${f}: $id is its file name`);
  assert.match(s.title, /format 1$/, `${f}: title names its format`);
}

// Every valid fixture validates; every invalid one fails, at the place cases.json names. cases.json lists them all.
let checked = 0;
for (const kind of Object.keys(schemas)) {
  const validate = schemas[kind];
  const dir = `../fixtures/sfi/${kind}/`;
  const valid = readdirSync(new URL(`${dir}valid/`, here)).filter((n) => n.endsWith(".json"));
  assert(valid.length > 0, `${kind}: valid fixtures`);
  for (const n of valid) { assert(validate(read(`${dir}valid/${n}`)), `${kind}/valid/${n}: ${errs(validate)}`); checked++; }
  const cases = read(`${dir}invalid/cases.json`);
  const invalid = readdirSync(new URL(`${dir}invalid/`, here)).filter((n) => n.endsWith(".json") && n !== "cases.json");
  assert.deepEqual(invalid.sort(), Object.keys(cases).sort(), `${kind}: cases.json lists exactly the invalid fixtures`);
  for (const n of invalid) {
    assert.equal(validate(read(`${dir}invalid/${n}`)), false, `${kind}/invalid/${n} must not validate (${cases[n].why})`);
    const at = cases[n].path;
    // "" means the record itself (a missing or unknown field), not "anywhere"
    assert(validate.errors.some((e) => e.instancePath === at || (at !== "" && e.instancePath.startsWith(`${at}/`))),
      `${kind}/invalid/${n}: no error at ${JSON.stringify(at)} (${cases[n].why}): ${errs(validate)}`);
    checked++;
  }
}

// ---- state: bounds a fixture file would be too big for
{
  const v = schemas.state;
  const base = read("../fixtures/sfi/state/valid/running-qs-skipped.json");
  const pkg = base.packages[0];
  const many = (n) => ({ ...structuredClone(base), packages: Array.from({ length: n }, (_, i) => ({ ...pkg, id: `p${i}` })) });
  assert(v(many(4096)), errs(v));
  assert.equal(v(many(4097)), false, "4097 packages");
  assert.equal(v({ ...structuredClone(base), launch: { ...base.launch, pid: -1 } }), false, "negative pid");
  for (const [field, value] of [["archivePresent", "yes"], ["runtimeActive", 0]]) {
    const d = structuredClone(base); d.packages[0][field] = value;
    assert.equal(v(d), false, `${field} is true, false or null`);
  }
  // every data and lua word the reducer may write is accepted
  for (const data of ["not-needed", "staged", "prepared", "committed", "cached-verified", "verified-restored", "excluded", "failed", "uncertain"]) {
    const d = structuredClone(base);
    Object.assign(d.packages[0], { data, archivePresent: data === "uncertain" ? null : ["committed", "cached-verified"].includes(data) });
    assert(v(d), `data ${data}: ${errs(v)}`);
  }
  for (const lua of ["not-needed", "admitted", "ready", "active", "failed", "excluded", "stopped"]) {
    const d = structuredClone(base);
    Object.assign(d.packages[0], { lua, runtimeActive: lua === "active" });
    assert(v(d), `lua ${lua}: ${errs(v)}`);
  }
  // An aside record's carried list has the packages' bound
  const aside = read("../fixtures/sfi/state/valid/aside-carried.json");
  const entry = aside.carriedSaveSensitive[0];
  const carrying = (n) => ({ ...structuredClone(aside), carriedSaveSensitive: Array.from({ length: n }, (_, i) => ({ ...entry, id: `p${i}` })) });
  assert(v(carrying(4096)), errs(v));
  assert.equal(v(carrying(4097)), false, "4097 carried packages");
  assert(v({ ...structuredClone(aside), carriedSaveSensitive: [] }), `an empty carried list: ${errs(v)}`);
}

// ---- state: what kiln-sfi's writer (crates/sfi/kiln-sfi/src/state) actually writes. Its test
// (kiln-sfi tests/state.rs the_writer_produces_the_schema_fixtures) keeps these byte-identical to its output.
{
  const dir = "../fixtures/sfi/state/writer/";
  const written = readdirSync(new URL(dir, here)).filter((n) => n.endsWith(".json"));
  assert(written.length >= 10, `the writer's documents: ${written.length}`);
  for (const n of written) {
    const doc = read(`${dir}${n}`);
    assert(schemas.state(doc), `state/writer/${n}: ${errs(schemas.state)}`);
    assert.equal(doc.writer, "kiln-sfi", n);
    checked++;
  }
}

// ---- state: the reader rules the schema description names (launchId against kiln.launch needs the live table)
const stateRules = (doc, what) => {
  const ids = doc.packages.map((p) => p.id);
  assert.equal(new Set(ids).size, ids.length, `${what}: a package id twice`);
  const carried = (doc.carriedSaveSensitive ?? []).map((p) => p.id);
  assert.equal(new Set(carried).size, carried.length, `${what}: a carried package id twice`);
  for (const p of doc.packages) {
    if (p.warning) assert(p.warning.revision <= doc.warningRevision, `${what}: ${p.id}: warning revision above warningRevision`);
  }
};
{
  for (const n of readdirSync(new URL("../fixtures/sfi/state/valid/", here))) stateRules(read(`../fixtures/sfi/state/valid/${n}`), n);
  const base = read("../fixtures/sfi/state/valid/running-qs-skipped.json");
  assert.throws(() => stateRules({ ...base, packages: [base.packages[0], base.packages[0]] }, "dup"), /a package id twice/);
  for (const n of readdirSync(new URL("../fixtures/sfi/state/writer/", here))) stateRules(read(`../fixtures/sfi/state/writer/${n}`), n);
  const aside = read("../fixtures/sfi/state/valid/aside-carried.json");
  const twice = { ...aside, carriedSaveSensitive: [aside.carriedSaveSensitive[0], aside.carriedSaveSensitive[0]] };
  assert.throws(() => stateRules(twice, "carried dup"), /a carried package id twice/);
  const ahead = structuredClone(base);
  const warned = ahead.packages.find((p) => p.warning);
  assert(warned, "running-qs-skipped has a warning");
  warned.warning.revision = ahead.warningRevision + 1;
  assert.throws(() => stateRules(ahead, "ahead"), /above warningRevision/);
}

// ---- safety policy: the shipped file and the matching rule
// reader rule: exact id, exact version, every file of one variant byte-identical; a file is looked up on disk
// case-insensitively (paths are ASCII by the schema), so any file whose name differs only in case may be the one
const policyMatch = (policy, pkg) => {
  const onDisk = (path) => Object.entries(pkg.files).filter(([name]) => name.toLowerCase() === path.toLowerCase()).map(([, f]) => f);
  return policy.packages.find((row) => row.id === pkg.id && row.version === pkg.version &&
    row.variants.some((variant) => variant.files.every((f) => onDisk(f.path).some((d) => d.sha256 === f.sha256 && d.size === f.size)))) ?? null;
};
const policyRules = (policy, what) => {  // reader rules: one row per id+version, one entry per path in a variant
  const rows = new Set();
  for (const row of policy.packages) {
    const key = `${row.id}\u0000${row.version}`;
    assert(!rows.has(key), `${what}: ${row.id} ${row.version} listed twice`);
    rows.add(key);
    for (const variant of row.variants) {
      const paths = variant.files.map((f) => f.path.toLowerCase());
      assert.equal(new Set(paths).size, paths.length, `${what}: ${row.id}: a path twice in one variant`);
    }
  }
};
{
  const v = schemas.safety;
  const shipped = read("../sfi/sfi-safety-policy.json");
  assert(v(shipped), `sfi/sfi-safety-policy.json: ${errs(v)}`);
  policyRules(shipped, "shipped policy");
  for (const n of readdirSync(new URL("../fixtures/sfi/safety/valid/", here))) policyRules(read(`../fixtures/sfi/safety/valid/${n}`), n);
  // the first row: Quiver & Spellbook 0.8.2 (Nexus 240), identity and hashes from the local corpus
  const qs = shipped.packages[0];
  assert.equal(qs.id, "ammo-slots");
  assert.equal(qs.version, "0.8.2");
  assert.equal(qs.risk, "save-data-loss");
  assert.deepEqual(qs.source, { site: "nexus", modId: 240 });
  assert.deepEqual(qs.variants, [{ files: [
    { path: "mod.json", sha256: "f4f645892b374eb87f7fe223402bc301270122b7693d65182b37a0d9f478dc37", size: 580 },
    { path: "ammo-slots.dll", sha256: "40321287f8e4dd4787d0a5f5f778af030cd784b31107a340408c77fffd271fe2", size: 396800 },
  ] }]);
  assert.match(qs.text, /drops the quiver and spellbook ammo at the next save/);
  // A synthetic copy that declares the same hashes matches; nothing about the name, a near version or other bytes does.
  const copy = { id: "ammo-slots", version: "0.8.2", name: "anything", files: Object.fromEntries(qs.variants[0].files.map((f) => [f.path, { sha256: f.sha256, size: f.size }])) };
  assert.equal(policyMatch(shipped, copy), qs);
  assert.equal(policyMatch(shipped, { ...copy, files: { ...copy.files, "README.txt": { sha256: "0".repeat(64), size: 1 } } }), qs, "extra files do not matter");
  // a path is looked up on disk case-insensitively, as Windows does : Ammo-Slots.dll is ammo-slots.dll
  const cased = { ...copy, files: { "MOD.json": copy.files["mod.json"], "Ammo-Slots.dll": copy.files["ammo-slots.dll"] } };
  assert.equal(policyMatch(shipped, cased), qs, "file names differing in case on disk");
  for (const [what, pkg] of [
    ["another id with the display name", { ...copy, id: "quiver-and-spellbook", name: "Quiver & Spellbook" }],
    ["id differing in case", { ...copy, id: "Ammo-Slots" }],
    ["id containing the row's id", { ...copy, id: "ammo-slots-extra" }],
    ["version 0.8.3", { ...copy, version: "0.8.3" }],
    ["version v0.8.2", { ...copy, version: "v0.8.2" }],
    ["changed dll", { ...copy, files: { ...copy.files, "ammo-slots.dll": { sha256: "0".repeat(64), size: 396800 } } }],
    ["changed manifest size", { ...copy, files: { ...copy.files, "mod.json": { ...copy.files["mod.json"], size: 581 } } }],
    ["dll missing", { ...copy, files: { "mod.json": copy.files["mod.json"] } }],
  ]) assert.equal(policyMatch(shipped, pkg), null, what);
  // the reader refuses what the schema allows but would make matching ambiguous
  assert.throws(() => policyRules({ packages: [qs, qs] }, "dup"), /listed twice/);
  assert.throws(() => policyRules({ packages: [{ ...qs, variants: [{ files: [...qs.variants[0].files, { ...qs.variants[0].files[0], path: "MOD.json" }] }] }] }, "dup"), /a path twice/);
}

// ---- ui envelope: notice keys (the SF 1.7.9 publish rule: key = modId-id, file = sha256(key), id = modId.id)
const noticeRules = (doc, what) => {
  if (doc.kind !== "notice") return;
  const local = doc.notice.id.slice(doc.notice.modId.length + 1);
  assert(doc.notice.id.startsWith(`${doc.notice.modId}.`) && local.length > 0, `${what}: notice.id is modId.id`);
  assert.equal(doc.key, `${doc.notice.modId}-${local}`, `${what}: key is modId-id`);
  assert.equal(doc.keySha256, sha(doc.key), `${what}: keySha256 is sha256(key)`);
};
{
  const v = schemas.ui;
  for (const n of readdirSync(new URL("../fixtures/sfi/ui/valid/", here))) noticeRules(read(`../fixtures/sfi/ui/valid/${n}`), n);
  const a = read("../fixtures/sfi/ui/valid/notice-collision-1.json");
  const b = read("../fixtures/sfi/ui/valid/notice-collision-2.json");
  assert.equal(a.keySha256, b.keySha256, "SF's collision: a-b/c and a/b-c share one notice file");
  const n = read("../fixtures/sfi/ui/valid/notice.json");
  assert.throws(() => noticeRules({ ...n, keySha256: "0".repeat(64) }, "x"), /sha256/);
  assert.throws(() => noticeRules({ ...n, key: "world-editor-other" }, "x"), /modId-id/);
  // bounds: SF counts bytes; the schema's code-point limits are the loose side of the same bound
  const act = read("../fixtures/sfi/ui/valid/action-with-value.json");
  assert(v({ ...act, value: "v".repeat(1048576) }), errs(v));
  assert.equal(v({ ...act, value: "v".repeat(1048577) }), false, "value over 1 MiB");
  assert(v({ ...n, notice: { ...n.notice, title: "t".repeat(120), message: "m".repeat(2000) } }), errs(v));
  for (const id of ["a", "A.b-c_9", "x".repeat(80)]) assert(v({ ...act, modId: id, action: id }), `${id}: ${errs(v)}`);
  for (const id of ["", "a/b", "ä", "a\n"]) assert.equal(v({ ...act, action: id }), false, JSON.stringify(id));
}

// ---- ui envelope: the bytes on disk (Kiln.Ui's writer and kiln-sfi's runtime both reproduce wire/ byte for byte)
{
  const v = schemas.ui;
  const schema = read("../sfi/sfi-ui-envelope.schema.json");
  const order = (kind) => {
    const req = schema.$defs[kind].required;
    return kind === "action" ? [...req.slice(0, req.indexOf("action") + 1), "value", ...req.slice(req.indexOf("action") + 1)] : req;
  };
  const dir = new URL("../fixtures/sfi/ui/wire/", here);
  const wire = readdirSync(dir).filter((n) => n.endsWith(".json"));
  assert(wire.length >= 8, `wire fixtures: ${wire.length}`);
  const kinds = new Set();
  for (const n of wire) {
    const bytes = readFileSync(new URL(n, dir));
    assert.notEqual(bytes[0], 0xef, `${n}: no BOM`);
    const text = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
    const doc = JSON.parse(text);
    assert(v(doc), `ui/wire/${n}: ${errs(v)}`);
    noticeRules(doc, n);
    kinds.add(doc.kind);
    // members in the schema's order (absent optional members skipped), no whitespace, serde_json's escaping
    assert.deepEqual(Object.keys(doc), order(doc.kind).filter((k) => k in doc), `${n}: member order`);
    if (doc.kind === "notice") assert.deepEqual(Object.keys(doc.notice), schema.$defs.notice.properties.notice.required, `${n}: notice member order`);
    assert.equal(text, JSON.stringify(doc), `${n}: canonical bytes`);
    checked++;
  }
  assert.deepEqual([...kinds].sort(), ["action", "launch", "notice"], "wire/ has every kind");
  // the pretty valid/ twins say the same thing
  for (const n of ["action-with-value.json", "action-without-value.json", "notice.json", "notice-collision-1.json"])
    assert.deepEqual(read(`../fixtures/sfi/ui/wire/${n}`), read(`../fixtures/sfi/ui/valid/${n}`), n);
  assert.deepEqual(read("../fixtures/sfi/ui/wire/launch.json"), read("../fixtures/sfi/ui/valid/launch-receipt.json"));
  // the escapes the writers must agree on are all there
  const esc = readFileSync(new URL("action-escapes.json", dir), "utf8");
  for (const s of ['\\"', "\\\\", "\\n", "\\r", "\\t", "\\b", "\\f", "\\u0001", "\\u001f", "\u007f", "\u2028", "</script>", "😀"])
    assert(esc.includes(s), `action-escapes.json holds ${JSON.stringify(s)}`);

  // the bounds: one file, and the schema says the same numbers
  const b = read("../fixtures/sfi/ui/bounds.json");
  assert.deepEqual(Object.keys(b), ["format", "about", "recordBytes", "pendingActionsPerOwner", "noticeKeysPerProfile", "entriesPerTurn", "receiptBytes", "keepEndedLaunchDays"]);
  assert.equal(b.recordBytes, 2 * 1024 * 1024);
  assert.equal(b.entriesPerTurn, 32, "SF's take(32)");
  for (const s of ["2 MiB", `${b.pendingActionsPerOwner} pending actions per owner`, `${b.noticeKeysPerProfile} notice keys per profile`, `up to ${b.entriesPerTurn} entries`, `${b.keepEndedLaunchDays} days`])
    assert(schema.description.includes(s), `the schema's description says ${s}`);
  assert(schema.$defs.action.properties.value.maxLength * 1 <= b.recordBytes);

  // the drain cases: well formed, every wire file they name exists
  const drain = read("../fixtures/sfi/ui/drain/cases.json");
  const names = drain.cases.map((c) => c.name);
  assert.equal(new Set(names).size, names.length, "drain case names are unique");
  for (const c of drain.cases) {
    const entries = c.entries.map((e) => e.name);
    assert.equal(new Set(entries).size, entries.length, `${c.name}: entry names are unique`);
    for (const e of c.entries) {
      assert.equal(["wire", "text", "folder"].filter((k) => k in e).length, 1, `${c.name}/${e.name}: one form`);
      if (e.wire) assert(wire.includes(e.wire), `${c.name}/${e.name}: wire/${e.wire}`);
    }
    assert(c.turns.length > 0 && c.turns.every((t) => ("remaining" in t) !== ("remainingCount" in t)), `${c.name}: each turn expects remaining or remainingCount`);
    for (const t of c.turns) for (const r of t.remaining ?? []) assert(entries.includes(r), `${c.name}: remaining ${r} is an entry`);
    for (const t of c.turns) for (const r of t.locked ?? []) assert(entries.includes(r), `${c.name}: locked ${r} is an entry`);
    checked++;
  }
  assert(drain.cases.some((c) => c.entries.length > b.entriesPerTurn), "a case with more entries than one turn reads");
  assert(drain.cases.some((c) => c.entries.length === b.entriesPerTurn), "a case with exactly one turn's entries");
  assert(drain.cases.some((c) => c.turns.some((t) => t.locked?.length)), "a case with an entry held without sharing");
  assert(drain.rules.some((r) => r.includes("ignores launch.json")), "the drain rules state the receipt contract");
}

console.log(`sfi schema checks passed (${checked} fixtures)`);
