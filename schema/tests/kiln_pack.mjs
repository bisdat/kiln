// SPDX-License-Identifier: MIT
// kiln-modpack.schema.json (format 1 and format 2, the Crucible pack) and kiln-pack-instance.schema.json against their
// examples and a few bad documents. Uses the launcher's ajv:  node schema/tests/kiln_pack.mjs
// ajv is looked up as in closed_dll_schemas.mjs: $AJV_2020, this tree's launcher/node_modules, the main checkout's.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
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
const ajv = new Ajv2020({ strict: false, validateFormats: false });
ajv.addSchema(read("../common.schema.json"));
const modpack = ajv.compile(read("../kiln-modpack.schema.json"));
const instance = ajv.compile(read("../kiln-pack-instance.schema.json"));
const must = (v, doc, what) => assert(v(doc), `${what}: ${JSON.stringify(v.errors)}`);
const mustNot = (v, doc, what) => assert.equal(v(doc), false, what);

const pack = read("../examples/kiln-pack.json");
must(modpack, read("../examples/kiln-modpack.json"), "format 1 example");
must(modpack, pack, "format 2 example");
must(instance, read("../examples/kiln-pack-instance.json"), "instance example");
// a format-2 document is checked by the format-2 rules, not format 1's (no server object needed)
const clone = () => structuredClone(pack);
mustNot(modpack, { ...clone(), kind: "server" }, "kind must be pack");
mustNot(modpack, { ...clone(), id: "Bad Id" }, "id is a mod id");
mustNot(modpack, { ...clone(), id: "x".repeat(60) }, "id at most 59 characters");
mustNot(modpack, { ...clone(), version: "one" }, "version is SemVer");
let d = clone(); d.mods[0].path = "../escape"; mustNot(modpack, d, "mod path");
d = clone(); d.mods[0].dep = "maybe"; mustNot(modpack, d, "dep");
d = clone(); d.mods[0].credit.source = "http://example.org"; mustNot(modpack, d, "credit source https");
d = clone(); d.layer.path = "elsewhere"; mustNot(modpack, d, "layer path");
d = clone(); d.overrides[0].mode = "append"; mustNot(modpack, d, "override mode");
d = clone(); d.overrides[0].path = "../../etc/passwd"; mustNot(modpack, d, "override path");
d = clone(); delete d.mods; mustNot(modpack, d, "mods required");
d = clone(); d.overrides[0].size = -1; mustNot(modpack, d, "override size");
d = clone(); d.overrides[0].size = 65537; mustNot(modpack, d, "override size at most 64 KiB");
d = clone(); d.server.settings.userGroups = [{ name: "Admin", password: "x" }]; mustNot(modpack, d, "server settings: gameSettings only");
d = clone(); d.server.settings.gameSettings = 3; mustNot(modpack, d, "server gameSettings is an object");
// format 1 still needs its server object
const f1 = read("../examples/kiln-modpack.json"); delete f1.server; mustNot(modpack, f1, "format 1 server required");
mustNot(instance, { ...read("../examples/kiln-pack-instance.json"), sha256: "nope" }, "instance sha256");
console.log("kiln_pack.mjs: ok");
