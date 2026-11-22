# SPDX-License-Identifier: MIT
"""An authored NPC's assets are staged, never independently loaded by kiln.assets."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path[:0] = [str(ROOT), str(ROOT / "samples"), str(ROOT.parent / "kfc"), str(ROOT.parent / "modelcook")]
import make_golem
from anicook import blocks as B, recipe as R
from kfc.refl import TypeDb

TYPES = TypeDb.load(ROOT / "types" / "anim-types.json")
ROLES = ["sit", "sleep", "inspect", "greet", "talk", "reaction0", "reaction1", "refuse", "idleGesture0", "idleGesture1", "interaction", "greetingExtra", "farewell", "dialogOption", "attentionGreeting", "turnLeftSmall", "turnLeftBig", "turnRightSmall", "turnRightBig", "jump", "door"]


def motions():
    return {"bindings": [dict({"role": role, "variant": v, "state": f"{role}-{v}"}, **({"interactionKey": "81ed77f9-4c72-4730-8b05-a5ddee9c7164"} if role == "interaction" else {}))
                         for role in ROLES for v in range(3 if role in ("sit", "sleep", "door") else 1)]}


class NpcBodies(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.mod = Path(self.tmp.name)
        for d in ("models", "blocks", "assets"):
            (self.mod / d).mkdir()
        make_golem.write(self.mod / "models/golem.glb")
        (self.mod / "kiln.mod.json").write_text(json.dumps({"id": "npc-mod", "clients": "mod"}))
        self.recipe = self.mod / "assets/cook.json"
        self.recipe.write_text(json.dumps({"format": 1, "tool": "animcook", "steps": []}))
        self.doc = {"format": 1, "npcs": {"tobin": {"name": "Tobin", "body": {"model": "models/golem.glb", "size": {"radius": 0.75, "height": 2}, "slots": {"Root": "root", "Head": "head", "HandR": "hand_r"}},
                    "animation": {"clips": "models/golem.glb", "idle": "Idle", "walk": "WalkF", "run": "Run", "states": {b["state"]: {"clip": "Step", "loop": b["role"] in ("sit", "sleep") and b["variant"] == 1} for b in motions()["bindings"]}}, "motions": motions()}}}
        self.write()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self):
        (self.mod / "blocks/npc.json").write_text(json.dumps(self.doc))

    def test_animation_assets_are_only_in_the_body_manifest(self):
        rep = R.Run(self.recipe, TYPES).run(update_manifest=True)
        self.assertTrue(rep["ok"], rep)
        ordinary = json.loads((self.mod / "assets/assets.json").read_text())
        self.assertEqual(ordinary["entries"], [])
        bundle = json.loads((self.mod / "assets/npc-bodies.v1.json").read_text())
        body = bundle["bodies"]["tobin"]
        self.assertEqual(len(body["bindings"]), 27)
        self.assertTrue(all(b["durationNs"] > 0 for b in body["bindings"]))
        self.assertFalse(body["complete"], "modelcook has not run yet; this is not a publishable body")
        self.assertTrue(bundle["entries"])
        self.assertTrue(all(len(e["sha256"]) == 64 for e in bundle["entries"]))
        self.assertTrue(R.Run(self.recipe, TYPES).run()["ok"], "the split manifest validates without rewriting")

    def test_existing_creature_manifest_is_unchanged(self):
        self.doc["creatures"] = self.doc.pop("npcs")
        del self.doc["creatures"]["tobin"]["motions"]
        self.write()
        run = R.Run(self.recipe, TYPES)
        self.assertTrue(run.run(update_manifest=True)["ok"])
        self.assertEqual(json.loads((self.mod / "assets/assets.json").read_text())["entries"], run.entries)
        self.assertFalse((self.mod / "assets/npc-bodies.v1.json").exists())

    def stage_model(self):
        # The model emitter's output is synthetic: the split seam needs no installed game.
        model = self.mod / "assets/cooked/models_golem.model.bin"
        blob = self.mod / "assets/cooked/models_golem.render.blob"
        model.write_bytes(b"model")
        blob.write_bytes(b"blob")
        side = {"guid": "8bb42bf4-b129-511d-b210-f91bff63d45a", "skeleton": json.loads((self.mod / "assets/cooked/tobin.animation.tobin.body.graph.json").read_text())["skeleton"],
                "boundingBox": {"min": {"x": -0.2, "y": 0, "z": -0.2}, "max": {"x": 0.2, "y": 1.8, "z": 0.2}}, "materials": []}
        (self.mod / "assets/cooked/models_golem.model.json").write_text(json.dumps(side))
        entries = [{"kind": "resource", "guid": side["guid"], "type": "keen::RenderModel", "part": 0, "file": "cooked/models_golem.model.bin", "mode": "new"}, {"kind": "blob", "file": "cooked/models_golem.render.blob"}]
        return entries

    def test_both_cooks_merge_the_same_deferred_group(self):
        R.Run(self.recipe, TYPES).run(update_manifest=True)
        entries = self.stage_model()
        normal, missing, mismatched = B.body_manifest(self.recipe, entries, True)
        self.assertEqual((normal, missing, mismatched), ([], [], []))
        bundle = json.loads((self.mod / "assets/npc-bodies.v1.json").read_text())
        self.assertTrue(bundle["bodies"]["tobin"]["complete"])
        self.assertTrue(all(e["file"].startswith("assets/") for e in bundle["entries"]))
        before = (self.mod / "assets/npc-bodies.v1.json").read_bytes()
        B.body_manifest(self.recipe, entries, True)
        self.assertEqual(before, (self.mod / "assets/npc-bodies.v1.json").read_bytes())

    def test_a_server_preserves_the_shipped_expected_manifest_when_render_bytes_are_skipped(self):
        R.Run(self.recipe, TYPES).run(update_manifest=True)
        entries = self.stage_model()
        B.body_manifest(self.recipe, entries, True)
        path = self.mod / "assets/npc-bodies.v1.json"
        # Pin an old timestamp: the test does not rely on filesystem clock granularity.
        import os
        os.utime(path, ns=(1_000_000_000, 1_000_000_000))
        before, stamp = path.read_bytes(), path.stat().st_mtime_ns
        for name in ("models_golem.model.bin", "models_golem.render.blob", "models_golem.model.json"):
            (self.mod / "assets/cooked" / name).unlink()
        self.assertTrue(R.Run(self.recipe, TYPES).run(update_manifest=True)["ok"])
        self.assertEqual(before, path.read_bytes(), "animcook must preserve the author's model hashes and metadata")
        self.assertEqual(([], [], []), B.body_manifest(self.recipe, entries, True))
        self.assertEqual(before, path.read_bytes(), "the server's skipped model step must not replace expected hashes with null")
        self.assertEqual(stamp, path.stat().st_mtime_ns, "an unchanged shipped manifest must not be rewritten")

    def test_an_unrelated_cook_cannot_bless_corrupt_model_bytes_by_rehashing_them(self):
        R.Run(self.recipe, TYPES).run(update_manifest=True)
        entries = self.stage_model()
        B.body_manifest(self.recipe, entries, True)
        path = self.mod / "assets/npc-bodies.v1.json"
        before = path.read_bytes()
        (self.mod / "assets/cooked/models_golem.model.bin").write_bytes(b"corrupt, not the modelcook output")
        report = R.Run(self.recipe, TYPES).run(update_manifest=True)
        self.assertFalse(report["ok"], "animcook did not rebuild the model and cannot establish its new expected hash")
        self.assertTrue(report["manifest"]["mismatched"])
        self.assertEqual(before, path.read_bytes())

    def test_changed_body_sources_do_not_reuse_the_servers_shipped_ready_spec(self):
        R.Run(self.recipe, TYPES).run(update_manifest=True)
        entries = self.stage_model()
        B.body_manifest(self.recipe, entries, True)
        for name in ("models_golem.model.bin", "models_golem.render.blob", "models_golem.model.json"):
            (self.mod / "assets/cooked" / name).unlink()
        self.doc["npcs"]["tobin"]["body"]["size"]["radius"] = 1
        self.write()
        R.Run(self.recipe, TYPES).run(update_manifest=True)
        bundle = json.loads((self.mod / "assets/npc-bodies.v1.json").read_text())
        self.assertFalse(bundle["bodies"]["tobin"]["complete"])

    def test_manifest_does_not_follow_a_link(self):
        target = self.mod / "outside.json"
        target.write_text("untouched")
        (self.mod / "assets/npc-bodies.v1.json").symlink_to(target)
        with self.assertRaises((B.BlocksError, R.RecipeError)):
            R.Run(self.recipe, TYPES).run(update_manifest=True)
        self.assertEqual(target.read_text(), "untouched")
