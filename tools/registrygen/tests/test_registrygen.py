#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests of tools/registrygen/registrygen.py: schema checks, the ids lock, migrations, generated outputs.

    python3 -I tools/registrygen/tests/test_registrygen.py        (exit 0 = every test passed)
"""
import copy
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))

import registrygen as rg  # noqa: E402

MINI = {
    "registrySchema": 1, "registry": "things", "typeName": "Thing", "version": "1.0.0",
    "stability": "experimental", "experimentalId": "KILNX031", "doc": "Test things.",
    "ids": {"game": "game:<stable name>", "mod": "<mod id>:<name>", "rule": "Lower-case debug names."},
    "traits": {},
    "fields": {
        "size": {"type": "int", "min": 1, "max": 10, "applies": "worldLoad", "ops": "number", "doc": "Size."},
        "color": {"type": "enum", "values": ["red", "blue"], "applies": "now", "ops": "set"},
        "parts": {"type": "struct", "applies": "restart", "fields": {
            "a": {"type": "float", "min": 0, "max": 1, "ops": "number"}, "b": {"type": "bool", "ops": "set", "applies": "now"}}},
        "tags": {"type": "list", "of": {"type": "ref", "registry": "things"}, "maxItems": 3, "applies": "now", "ops": "list"},
        "rows": {"type": "list", "applies": "now", "ops": "list", "of": {"type": "struct", "typeName": "ThingRow", "fields": {
            "n": {"type": "int", "min": 0, "max": 9, "ops": "number"}, "inner": {"type": "struct", "fields": {"x": {"type": "float", "min": 0, "max": 1, "ops": "number"}}}}}},
        "fixed": {"type": "int", "min": 0, "max": 5, "readOnly": True, "applies": "restart"},
    },
    "entry": {"add": True, "remove": "hidden"},
    "raw": "allowed",
}


class Tree:
    """A made-up repository root with schema/registry/things.json."""

    def __init__(self, schema=None, lock=None):
        self.root = Path(tempfile.mkdtemp(prefix="registrygen-test-"))
        (self.root / "schema" / "registry").mkdir(parents=True)
        self.write(schema or MINI)
        if lock is not None:
            (self.root / "schema" / "registry" / "ids.lock.json").write_text(json.dumps(lock))

    def write(self, schema, name="things"):
        (self.root / "schema" / "registry" / f"{name}.json").write_text(json.dumps(schema))

    def lock(self):
        return json.loads((self.root / "schema" / "registry" / "ids.lock.json").read_text())

    def close(self):
        shutil.rmtree(self.root, ignore_errors=True)


class SchemaChecks(unittest.TestCase):
    def errors_of(self, schema):
        t = Tree(schema)
        try:
            return rg.load(t.root).errors
        finally:
            t.close()

    def test_mini_is_valid(self):
        self.assertEqual(self.errors_of(MINI), [])

    def test_leaf_without_applies_is_refused(self):
        s = copy.deepcopy(MINI)
        del s["fields"]["size"]["applies"]
        errs = self.errors_of(s)
        self.assertTrue(any("things.size" in e and "applies" in e for e in errs), errs)

    def test_struct_leaf_inherits_applies(self):
        m = rg.load(Tree(MINI).root)
        by = {f.path: f for f in m.registries["things"].leaves}
        self.assertEqual(by["parts.a"].applies, "restart")
        self.assertEqual(by["parts.b"].applies, "now")
        self.assertEqual(by["rows[].inner.x"].applies, "now")

    def test_ops_must_fit_the_type(self):
        s = copy.deepcopy(MINI)
        s["fields"]["size"]["ops"] = "list"
        self.assertTrue(any("things.size" in e and "ops" in e for e in self.errors_of(s)))
        s = copy.deepcopy(MINI)
        s["fields"]["color"]["ops"] = "number"
        self.assertTrue(any("things.color" in e for e in self.errors_of(s)))

    def test_read_only_takes_no_ops(self):
        s = copy.deepcopy(MINI)
        s["fields"]["fixed"]["ops"] = "number"
        self.assertTrue(any("things.fixed" in e and "read-only" in e for e in self.errors_of(s)))

    def test_ref_to_an_unknown_registry(self):
        s = copy.deepcopy(MINI)
        s["fields"]["tags"]["of"]["registry"] = "nowhere"
        self.assertTrue(any("nowhere" in e for e in self.errors_of(s)))

    def test_kiln_names_only(self):
        s = copy.deepcopy(MINI)
        s["fields"]["Size"] = s["fields"].pop("size")
        self.assertTrue(any("Size" in e for e in self.errors_of(s)))
        s = copy.deepcopy(MINI)
        s["fields"]["color"]["values"] = ["Red", "blue"]
        self.assertTrue(any("Red" in e for e in self.errors_of(s)))
        s = copy.deepcopy(MINI)
        s["fields"]["size"]["doc"] = "maxStackSize at +0x14"
        self.assertTrue(any("offset" in e for e in self.errors_of(s)))

    def test_every_number_has_a_range(self):
        for path in (("size",), ("parts", "fields", "a"), ("rows", "of", "fields", "n")):
            s = copy.deepcopy(MINI)
            node = s["fields"]
            for p in path[:-1]:
                node = node[p]
            del node[path[-1]]["max"]
            errs = self.errors_of(s)
            self.assertTrue(any("min and max" in e for e in errs), (path, errs))
        s = copy.deepcopy(MINI)
        s["fields"]["tags"] = {"type": "list", "of": {"type": "int", "min": 0}, "applies": "now", "ops": "list"}
        self.assertTrue(any("things.tags[]" in e and "min and max" in e for e in self.errors_of(s)))

    def test_ids_are_lower_case(self):
        s = copy.deepcopy(MINI)
        s["ids"]["game"] = "Game:<stable name>"
        self.assertTrue(any("ids.game" in e and "lower case" in e for e in self.errors_of(s)))

    def test_min_above_max(self):
        s = copy.deepcopy(MINI)
        s["fields"]["size"]["min"] = 20
        self.assertTrue(any("things.size" in e and "min" in e for e in self.errors_of(s)))

    def test_experimental_needs_its_id(self):
        s = copy.deepcopy(MINI)
        del s["experimentalId"]
        self.assertTrue(any("experimentalId" in e for e in self.errors_of(s)))

    def test_list_of_lists_is_refused(self):
        s = copy.deepcopy(MINI)
        s["fields"]["deep"] = {"type": "list", "applies": "now", "ops": "list", "of": {"type": "list", "of": {"type": "int", "min": 0, "max": 1}}}
        self.assertTrue(any("things.deep" in e for e in self.errors_of(s)))

    def test_flattened_paths(self):
        m = rg.load(Tree(MINI).root)
        paths = [f.path for f in m.registries["things"].rows]
        self.assertEqual(paths, ["size", "color", "parts.a", "parts.b", "tags", "rows", "rows[].n", "rows[].inner.x", "fixed"])


class Lock(unittest.TestCase):
    def setUp(self):
        self.t = Tree()

    def tearDown(self):
        self.t.close()

    def test_unlocked_names_fail_the_check(self):
        m = rg.load(self.t.root)
        errs = rg.lock_errors(m, rg.read_lock(self.t.root))
        self.assertTrue(any("things.size" in e and "ids.lock.json" in e for e in errs), errs)

    def test_lock_assigns_ids_in_order(self):
        rg.update_lock(self.t.root)
        lock = self.t.lock()
        reg = lock["registries"]["things"]
        self.assertEqual(reg["id"], 1)
        self.assertEqual(reg["fields"]["size"]["id"], 1)
        self.assertEqual(reg["fields"]["fixed"]["id"], 9)
        self.assertEqual(reg["fields"]["size"]["since"], "1.0.0")
        self.assertEqual(rg.lock_errors(rg.load(self.t.root), rg.read_lock(self.t.root)), [])

    def test_lock_is_stable(self):
        rg.update_lock(self.t.root)
        first = self.t.lock()
        rg.update_lock(self.t.root)
        self.assertEqual(first, self.t.lock())

    def test_removed_field_needs_retire(self):
        rg.update_lock(self.t.root)
        s = copy.deepcopy(MINI)
        del s["fields"]["color"]
        self.t.write(s)
        errs = rg.lock_errors(rg.load(self.t.root), rg.read_lock(self.t.root))
        self.assertTrue(any("things.color" in e and "retire" in e for e in errs), errs)
        rg.retire(self.t.root, "things.color", "1.1.0")
        self.assertEqual(rg.lock_errors(rg.load(self.t.root), rg.read_lock(self.t.root)), [])
        self.assertEqual(self.t.lock()["registries"]["things"]["retired"]["color"]["retiredIn"], "1.1.0")

    def test_retired_name_is_never_reused(self):
        rg.update_lock(self.t.root)
        rg.retire(self.t.root, "things.color", "1.1.0")
        errs = rg.lock_errors(rg.load(self.t.root), rg.read_lock(self.t.root))  # color is back in the schema
        self.assertTrue(any("things.color" in e and "retired" in e for e in errs), errs)
        with self.assertRaises(rg.LockError):
            rg.update_lock(self.t.root)

    def test_new_ids_never_reuse_retired_ones(self):
        rg.update_lock(self.t.root)
        s = copy.deepcopy(MINI)
        del s["fields"]["fixed"]
        self.t.write(s)
        rg.retire(self.t.root, "things.fixed", "1.1.0")
        s["fields"]["weight"] = {"type": "int", "min": 0, "max": 9, "applies": "now", "ops": "number"}
        s["version"] = "1.1.0"
        self.t.write(s)
        rg.update_lock(self.t.root)
        reg = self.t.lock()["registries"]["things"]
        self.assertEqual(reg["retired"]["fixed"]["id"], 9)
        self.assertEqual(reg["fields"]["weight"]["id"], 10)
        self.assertEqual(reg["fields"]["weight"]["since"], "1.1.0")

    def test_major_bump_needs_a_migration(self):
        rg.update_lock(self.t.root)
        s = copy.deepcopy(MINI)
        s["version"] = "2.0.0"
        self.t.write(s)
        errs = rg.lock_errors(rg.load(self.t.root), rg.read_lock(self.t.root))
        self.assertTrue(any("migrations/things-1.0.0-2.0.0.json" in e for e in errs), errs)
        mig = self.t.root / "schema" / "registry" / "migrations"
        mig.mkdir()
        (mig / "things-1.0.0-2.0.0.json").write_text(json.dumps({"registry": "things", "from": "1.0.0", "to": "2.0.0", "ids": {}, "fields": {}}))
        self.assertEqual(rg.lock_errors(rg.load(self.t.root), rg.read_lock(self.t.root)), [])

    def test_version_may_not_go_down(self):
        rg.update_lock(self.t.root)
        s = copy.deepcopy(MINI)
        s["version"] = "0.9.0"
        self.t.write(s)
        errs = rg.lock_errors(rg.load(self.t.root), rg.read_lock(self.t.root))
        self.assertTrue(any("0.9.0" in e for e in errs), errs)


class Outputs(unittest.TestCase):
    def setUp(self):
        self.t = Tree()
        rg.update_lock(self.t.root)
        self.out = rg.render(rg.load(self.t.root), rg.read_lock(self.t.root))

    def tearDown(self):
        self.t.close()

    def test_every_output_named(self):
        self.assertEqual(sorted(self.out), sorted([
            "native/services/content/registry/schema.g.h",
            "managed/services/Kiln.Registry/Generated/Things.gen.cs",
            "managed/services/Kiln.Registry/Generated/RegistrySchema.gen.cs",
            "schema/registry-section.schema.json",
            "docs/kiln/modding/registries/things.md",
        ]))

    def test_deterministic(self):
        again = rg.render(rg.load(self.t.root), rg.read_lock(self.t.root))
        self.assertEqual(self.out, again)

    def test_native_ids_follow_the_lock(self):
        h = self.out["native/services/content/registry/schema.g.h"]
        self.assertIn("things_size = 0x00010001u", h)
        self.assertIn("things_fixed = 0x00010009u", h)
        self.assertIn('"rows[].inner.x"', h)
        self.assertIn("Applies::WorldLoad", h)

    def test_csharp_carries_applies_on_every_leaf(self):
        cs = self.out["managed/services/Kiln.Registry/Generated/Things.gen.cs"]
        for path, applies in [("size", "WorldLoad"), ("color", "Now"), ("parts.a", "Restart"), ("parts.b", "Now"),
                              ("tags", "Now"), ("rows", "Now"), ("rows[].n", "Now"), ("rows[].inner.x", "Now"), ("fixed", "Restart")]:
            self.assertEqual(cs.count(f'[RegistryField("things.{path}", RegistryApplies.{applies})]'), 1, path)
        self.assertIn('CodeAnalysis.Experimental("KILNX031"', cs)
        self.assertIn("public sealed partial class ThingEntry", cs)
        self.assertIn("public sealed partial class ThingRow", cs)
        self.assertIn("public int Fixed => ", cs)  # read-only: no setter

    def test_section_schema_names_the_fields(self):
        s = json.loads(self.out["schema/registry-section.schema.json"])
        things = s["$defs"]["things"]
        self.assertEqual(sorted(things["$defs"]["field"]["enum"]), sorted(["size", "color", "parts.a", "parts.b", "tags", "rows", "rows[].n", "rows[].inner.x"]))

    def test_docs_show_applies(self):
        md = self.out["docs/kiln/modding/registries/things.md"]
        self.assertIn("| `size` | int 1..10 | worldLoad |", md)
        self.assertIn("Do not edit", md)
        self.assertIn("are reserved", md)
        self.assertIn("\x60game\x60 and \x60kiln\x60", md)

    def test_check_finds_stale_files(self):
        rg.write_outputs(self.t.root, self.out)
        self.assertEqual(rg.stale(self.t.root, self.out), [])
        p = self.t.root / "docs/kiln/modding/registries/things.md"
        p.write_text(p.read_text() + "edited\n")
        self.assertEqual(rg.stale(self.t.root, self.out), ["docs/kiln/modding/registries/things.md"])


class Repository(unittest.TestCase):
    def test_the_repository_is_current(self):
        """registrygen.py --check on this tree: the schemas are valid, every name is locked, the outputs are fresh."""
        self.assertEqual(rg.main(["--root", str(REPO), "--check"]), 0)

    def test_r1_registries(self):
        m = rg.load(REPO)
        self.assertEqual(m.errors, [])
        self.assertEqual(sorted(m.registries), ["balance", "drops", "items", "loot", "recipes"])
        for r in m.registries.values():
            for f in r.rows:
                self.assertIn(f.applies, ("now", "worldLoad", "restart"), f.qualified)


if __name__ == "__main__":
    unittest.main(verbosity=1)
