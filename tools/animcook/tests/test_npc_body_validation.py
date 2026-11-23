# SPDX-License-Identifier: MIT
"""Pure authoring checks, with synthetic rigs and clips only."""
import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path[:0] = [str(ROOT), str(ROOT.parent / "kfc"), str(ROOT.parent / "modelcook")]
from anicook import blocks as B, graphgen as G


def fixture():
    bindings = [{"role": role, "variant": v, "state": f"{role}-{v}"} for role in B.MOTION_ROLES
                for v in range(3 if role in ("sit", "sleep", "door") else 1)]
    next(b for b in bindings if b["role"] == "interaction")["interactionKey"] = B.FISHER_INTERACTION
    skeleton = G.derive("body-validation", "skeleton", "rig")
    animation = {"idle": "Pose", "states": {b["state"]: {"clip": "Pose", "speed": 2, "loop": b["role"] in ("sit", "sleep") and b["variant"] == 1} for b in bindings}}
    spec = {"skeletonName": "rig", "animation": animation, "motions": {"bindings": bindings}}
    graph = {"mod": "body-validation", "skeleton": skeleton, "states": {b["state"]: {"guid": G.derive("body-validation", "state", b["state"])} for b in bindings}}
    graph["states"]["Idle"] = {"guid": G.ENGINE_GUIDS["Idle"]}
    graph["states"]["Run"] = {"guid": None}
    clips = {"clips": {key: {"duration": 3, "skeleton": skeleton} for key in ("rig.Pose", "rig.Pose.once")}}
    return spec, graph, clips


class NpcBodyValidation(unittest.TestCase):
    def test_bindings_match_the_shared_managed_vector(self):
        vector = json.loads((ROOT.parent.parent / "mods/tests/npc-motion-contract/vector.json").read_text())
        actual = B._body_bindings(vector, vector["graph"], vector["clips"], "motions")
        self.assertEqual(vector["expected"], actual)

    def test_ordinary_body_redefinitions_keep_the_existing_cook_path(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "blocks").mkdir()
            (root / "blocks/a.json").write_text(json.dumps({"bodies": {"stock": {"like": "fisher"}}}))
            (root / "blocks/b.json").write_text(json.dumps({"bodies": {"stock": {"like": "villager"}}}))
            self.assertEqual({}, B.npc_bodies(root))
            self.assertEqual([], B.plan(root)["models"])

    def test_invalid_optional_fallback_is_a_field_error(self):
        spec, _, _ = fixture()
        for value in (False, 1, ["Idle"]):
            with self.subTest(value=value):
                with self.assertRaises(B.BlocksError) as error:
                    B._motion_shape(dict(spec["motions"], optionalFallback=value), "motions")
                self.assertIn("motions.optionalFallback", str(error.exception))

    def test_external_gltf_buffer_changes_invalidate_the_other_producers_source_hash(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for directory in ("models", "blocks", "assets/cooked"):
                (root / directory).mkdir(parents=True)
            recipe = root / "assets/cook.json"
            recipe.write_text('{"format":1,"steps":[]}')
            (root / "kiln.mod.json").write_text('{"id":"body-validation"}')
            (root / "models/rig.gltf").write_text('{"buffers":[{"uri":"rig.bin"}]}')
            buffer = root / "models/rig.bin"
            buffer.write_bytes(b"first authored rig")
            spec, _, _ = fixture()
            npc = {"name": "Tobin", "body": {"model": "models/rig.gltf", "size": {"radius": .75, "height": 2}, "slots": {"Root": "root", "Head": "head", "HandR": "hand"}},
                   "animation": dict(spec["animation"], clips="models/rig.gltf"), "motions": spec["motions"]}
            (root / "blocks/npc.json").write_text(json.dumps({"npcs": {"tobin": npc}}))
            (root / "assets/cooked/example.bin").write_bytes(b"synthetic resource")
            entries = [{"kind": "resource", "guid": G.derive("body-validation", "test", "resource"), "type": "test", "part": 0, "file": "cooked/example.bin", "mode": "new"}]
            B.body_manifest(recipe, entries, True, producer="animcook")
            first = json.loads((root / "assets/cooked/npc-bodies.v1.json").read_text())["entries"][0]["sourceHash"]
            buffer.write_bytes(b"changed authored rig")
            B.body_manifest(recipe, entries, True, producer="animcook")
            second = json.loads((root / "assets/cooked/npc-bodies.v1.json").read_text())["entries"][0]["sourceHash"]
            self.assertNotEqual(first, second)

    @staticmethod
    def uri_fixture(root, uri):
        for directory in ("models", "shared", "blocks", "assets/cooked"):
            (root / directory).mkdir(parents=True, exist_ok=True)
        recipe = root / "assets/cook.json"
        recipe.write_text('{"format":1,"steps":[]}')
        (root / "kiln.mod.json").write_text('{"id":"body-validation"}')
        (root / "models/rig.gltf").write_text(json.dumps({"buffers": [{"uri": uri}]}))
        spec, _, _ = fixture()
        npc = {"name": "Tobin", "body": {"model": "models/rig.gltf", "size": {"radius": .75, "height": 2},
               "slots": {"Root": "root", "Head": "head", "HandR": "hand"}},
               "animation": dict(spec["animation"], clips="models/rig.gltf"), "motions": spec["motions"]}
        (root / "blocks/npc.json").write_text(json.dumps({"npcs": {"tobin": npc}}))
        (root / "assets/cooked/example.bin").write_bytes(b"synthetic resource")
        entries = [{"kind": "resource", "guid": G.derive("body-validation", "test", "resource"),
                    "type": "test", "part": 0, "file": "cooked/example.bin", "mode": "new"}]
        return recipe, entries

    def test_external_gltf_uris_decode_spaces_and_normalize_in_mod_parents(self):
        for uri, file in (("rig%20data.bin", "models/rig data.bin"), ("../shared/rig.bin", "shared/rig.bin")):
            with self.subTest(uri=uri), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                recipe, entries = self.uri_fixture(root, uri)
                source = root / file
                source.write_bytes(b"first authored source")
                B.body_manifest(recipe, entries, True, producer="animcook")
                cache = root / "assets/cooked/npc-bodies.v1.json"
                first = json.loads(cache.read_text())["sourceHash"]
                source.write_bytes(b"changed authored source")
                B.body_manifest(recipe, entries, True, producer="animcook")
                self.assertNotEqual(first, json.loads(cache.read_text())["sourceHash"])

    def test_external_gltf_uris_still_refuse_a_decoded_escape_from_the_mod(self):
        for uri in ("../../outside.bin", "%2e%2e/%2e%2e/outside.bin", "link/../in-mod.bin"):
            with self.subTest(uri=uri), tempfile.TemporaryDirectory() as folder:
                base = Path(folder)
                root = base / "mod"
                recipe, entries = self.uri_fixture(root, uri)
                (base / "outside.bin").write_bytes(b"not a mod source")
                (base / "outside").mkdir()
                (root / "models/link").symlink_to(base / "outside", target_is_directory=True)
                (root / "models/in-mod.bin").write_bytes(b"normalizing first must not bypass the link check")
                with self.assertRaises(B.BlocksError):
                    B.body_manifest(recipe, entries, True, producer="animcook")
                self.assertFalse((root / "assets/cooked/npc-bodies.v1.json").exists())

    def test_malformed_body_cache_sources_metadata_and_materials_are_diagnostics(self):
        cases = [
            ("cache root", "assets/cooked/npc-bodies.v1.json", [], "npc-bodies.v1"),
            ("cache entry", "assets/cooked/npc-bodies.v1.json", {"format": 1, "entries": [1], "bodies": {}}, "entries"),
            ("glTF root", "models/rig.gltf", [], "rig.gltf"),
            ("buffer entry", "models/rig.gltf", {"buffers": [False]}, "buffers"),
            ("image entry", "models/rig.gltf", {"images": [1]}, "images"),
            ("buffer container", "models/rig.gltf", {"buffers": 42}, "buffers"),
            ("material document", "blocks/material.json", [], "material.json"),
            ("material section", "blocks/material.json", {"materials": ["surface"]}, "materials"),
            ("material block", "blocks/material.json", {"materials": {"surface": []}}, "materials.surface"),
            ("model metadata", "@model", [], "model.json"),
            ("graph metadata", "@graph", [], "graph.json"),
            ("clip metadata", "assets/cooked/clips.json", [], "clips.json"),
        ]
        for case, target, payload, field in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                recipe, entries = self.uri_fixture(root, "rig.bin")
                (root / "models/rig.bin").write_bytes(b"synthetic authored buffer")
                spec = B.npc_bodies(root)["tobin"]
                model = "assets/cooked/" + spec["modelName"].replace("/", "_") + ".model.json"
                graph = "assets/cooked/" + spec["graphName"] + ".graph.json"
                (root / model).write_text(json.dumps({"guid": G.derive("body-validation", "model", "rig"),
                    "materials": [G.derive("body-validation", "material", "surface")]}))
                target = model if target == "@model" else graph if target == "@graph" else target
                (root / target).write_text(json.dumps(payload))
                cache = root / "assets/cooked/npc-bodies.v1.json"
                before = cache.read_bytes() if cache.exists() else None
                with self.assertRaises(B.BlocksError) as error:
                    B.body_manifest(recipe, entries, True, producer="animcook")
                self.assertIn(field, str(error.exception))
                self.assertEqual(before, cache.read_bytes() if cache.exists() else None)

    def test_a_malformed_part_section_does_not_crash_an_ordinary_mod_plan(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "blocks").mkdir()
            (root / "blocks/parts.json").write_text('{"npcMotions":["not a part map"]}')
            self.assertEqual({}, B.npc_bodies(root))
            self.assertEqual([], B.plan(root)["models"])

    def test_incomplete_legacy_npc_models_keep_the_ordinary_eager_cook_path(self):
        import warnings
        for missing in ("animation", "motions"):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                recipe, entries = self.uri_fixture(root, "rig.bin")
                block = root / "blocks/npc.json"
                document = json.loads(block.read_text())
                document["npcs"]["tobin"].pop(missing)
                block.write_text(json.dumps(document))
                with warnings.catch_warnings(record=True) as notices:
                    warnings.simplefilter("always")
                    self.assertEqual({}, B.npc_bodies(root))
                self.assertTrue(any("npcs.tobin." + missing in str(w.message) for w in notices))
                with warnings.catch_warnings(record=True):
                    warnings.simplefilter("always")
                    planned = B.plan(root)
                self.assertTrue(any(step["gltf"] == "models/rig.gltf" for step in planned["models"]))
                self.assertFalse(B.body_step(recipe, {"op": "model", "name": "models/rig"}, specs={}))
                self.assertEqual((entries, [], []), B.body_manifest(recipe, entries, True, specs={}))

    def test_interaction_key_is_implicit_or_the_exact_supported_key(self):
        for mode in ("omitted", "null", "explicit"):
            with self.subTest(mode=mode):
                spec, graph, clips = fixture()
                row = next(b for b in spec["motions"]["bindings"] if b["role"] == "interaction")
                if mode == "omitted":
                    row.pop("interactionKey")
                elif mode == "null":
                    row["interactionKey"] = None
                B._motion_shape(spec["motions"], "motions")
                vector = B._body_bindings(spec, graph, clips, "motions")
                self.assertEqual(B.FISHER_INTERACTION, next(b for b in vector if b["role"] == "interaction")["interactionKey"])
        for value in ("", False, "00000000-0000-0000-0000-000000000000", "12345678-1234-5678-9234-123456789abc"):
            with self.subTest(invalid=value):
                spec, _, _ = fixture()
                next(b for b in spec["motions"]["bindings"] if b["role"] == "interaction")["interactionKey"] = value
                with self.assertRaisesRegex(B.BlocksError, "interactionKey"):
                    B._motion_shape(spec["motions"], "motions")

    def test_shared_interaction_schema_fixture_is_valid_motion_authoring(self):
        source = ROOT.parent.parent / "mods/tests/npc-motion-contract/blocks/motions.json"
        for name, motions in json.loads(source.read_text())["npcMotions"].items():
            with self.subTest(name=name):
                B._motion_shape(motions, name)

    def test_complete_vector_uses_effective_cooked_duration_and_canonical_order(self):
        spec, graph, clips = fixture()
        vector = B._body_bindings(spec, graph, clips, "motions")
        self.assertEqual(len(vector), 27)
        self.assertEqual(sum(row["loop"] for row in vector), 2)
        self.assertTrue(all(row["durationNs"] == 1_500_000_000 for row in vector))
        spec["motions"]["bindings"].reverse()
        self.assertEqual(vector, B._body_bindings(spec, graph, clips, "motions"))

    def test_unknown_role_variant_duplicate_and_missing_required_motion_are_refused(self):
        spec, _, _ = fixture()
        changes = [("role", lambda bs: bs[0].update(role="new-role")),
                   ("variant", lambda bs: bs[0].update(variant=3)),
                   ("duplicate", lambda bs: bs.append(copy.deepcopy(bs[0]))),
                   ("missing sit", lambda bs: bs.pop(0))]
        for field, change in changes:
            with self.subTest(field=field):
                motions = copy.deepcopy(spec["motions"])
                change(motions["bindings"])
                with self.assertRaises(B.BlocksError) as error:
                    B._motion_shape(motions, "motions")
                self.assertIn(field, str(error.exception))

    def test_optional_fallback_is_explicit_and_clamped(self):
        spec, graph, clips = fixture()
        spec["motions"]["bindings"] = [b for b in spec["motions"]["bindings"] if b["role"] in ("sit", "sleep", "inspect", "jump", "door")]
        with self.assertRaises(B.BlocksError):
            B._motion_shape(spec["motions"], "motions")
        spec["motions"]["optionalFallback"] = "Idle"
        B._motion_shape(spec["motions"], "motions")
        vector = B._body_bindings(spec, graph, clips, "motions")
        self.assertEqual(27, len(vector))
        self.assertTrue(all(not b["loop"] for b in vector if b["clamp"]))

    def test_a_missing_or_GUID_less_motion_target_refuses_with_its_field(self):
        for state in ("missing", "Run"):
            with self.subTest(state=state):
                spec, graph, clips = fixture()
                spec["motions"]["bindings"][0]["state"] = state
                with self.assertRaises(B.BlocksError) as error:
                    B._body_bindings(spec, graph, clips, "motions")
                self.assertIn("motions.sit[0].state", str(error.exception))

    def test_nullable_numeric_scale_has_the_same_shape_as_managed_JSON(self):
        body = {"size": {"radius": .75, "height": 2}, "slots": {"Root": "root", "Head": "head", "HandR": "hand"}}
        for scale in (None, 1, 1.0):
            with self.subTest(valid=scale):
                B._body_geometry(dict(body, scale=scale), "body")
        for scale in (True, False, "1"):
            with self.subTest(invalid=scale):
                with self.assertRaisesRegex(B.BlocksError, "body.scale"):
                    B._body_geometry(dict(body, scale=scale), "body")

    def test_body_size_ranges_scale_colliders_and_slots_are_checked(self):
        body = {"size": {"radius": .75, "height": 2}, "slots": {"Root": "root", "Head": "head", "HandR": "hand"}}
        B._body_geometry(body, "body")
        mutations = [("size", lambda b: b.pop("size")),
                     ("size.radius", lambda b: b["size"].update(radius=float("nan"))),
                     ("size.height", lambda b: b["size"].update(height=9)),
                     ("scale", lambda b: b.update(scale=2)),
                     ("colliders", lambda b: b.update(colliders=[{}])),
                     ("slots[Head]", lambda b: b["slots"].pop("Head"))]
        for field, change in mutations:
            with self.subTest(field=field):
                invalid = copy.deepcopy(body)
                change(invalid)
                with self.assertRaises(B.BlocksError) as error:
                    B._body_geometry(invalid, "body")
                self.assertIn("body." + field, str(error.exception))
