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
