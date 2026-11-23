// SPDX-License-Identifier: MIT
// Run from the repo root with node; uses the same locked Ajv packages as kiln-manage's lint cross-check.
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { readFileSync, readdirSync } from "node:fs";
import { join, resolve } from "node:path";

const root = resolve(process.argv[2] ?? ".");
const modules = resolve(process.env.KILN_AJV_MODULES ?? join(root, "launcher/node_modules"));
const require = createRequire(join(modules, "npc-motion-schema.js"));
const Ajv = require("ajv/dist/2020").default;
const addFormats = require("ajv-formats").default;
const { parse } = require("jsonc-parser");
const ajv = new Ajv({ allErrors: true, strict: false });
addFormats(ajv);
for (const name of readdirSync(join(root, "schema")).filter(name => name.endsWith(".schema.json")))
    ajv.addSchema(parse(readFileSync(join(root, "schema", name), "utf8"), [], { allowTrailingComma: true }), name);
const validate = ajv.getSchema("kiln-blocks.schema.json");
const fixture = JSON.parse(readFileSync(join(root, "mods/tests/npc-motion-contract/blocks/motions.json"), "utf8"));
let count = 0;
function check(motions, expected, name) {
    const valid = validate({ format: 1, npcMotions: { contract: motions } });
    assert.equal(valid, expected, `${name}: ${JSON.stringify(validate.errors)}`);
    count++;
}
for (const [name, motions] of Object.entries(fixture.npcMotions)) check(motions, true, name);
for (const key of ["", false, "00000000-0000-0000-0000-000000000000", "12345678-1234-5678-9234-123456789abc"]) {
    const motions = structuredClone(fixture.npcMotions.implicit);
    motions.bindings.find(row => row.role === "interaction").interactionKey = key;
    check(motions, false, `unsupported interaction key ${JSON.stringify(key)}`);
}
const wrongRole = structuredClone(fixture.npcMotions["explicit-key"]);
wrongRole.bindings[0].interactionKey = wrongRole.bindings.find(row => row.role === "interaction").interactionKey;
check(wrongRole, false, "a non-interaction role cannot take a key");
console.log(`NPC motion schema: ${count} cases passed`);
