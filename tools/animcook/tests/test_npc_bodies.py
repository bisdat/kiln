# SPDX-License-Identifier: MIT
"""An authored NPC's assets are staged, never independently loaded by kiln.assets."""
import contextlib
import io
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

    def test_was_uses_null_coalescing_like_managed_and_native_NPC_identity(self):
        for was, expected in ((None, "tobin"), ("", ""), ("old-tobin", "old-tobin")):
            with self.subTest(was=was):
                self.doc["npcs"]["tobin"]["was"] = was
                self.write()
                self.assertEqual(expected, B.npc_bodies(self.mod)["tobin"]["key"])

    def test_animation_assets_are_only_in_the_body_manifest(self):
        rep = R.Run(self.recipe, TYPES).run(update_manifest=True)
        self.assertTrue(rep["ok"], rep)
        ordinary = json.loads((self.mod / "assets/assets.json").read_text())
        self.assertEqual(ordinary["entries"], [])
        bundle = json.loads((self.mod / "assets/cooked/npc-bodies.v1.json").read_text())
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
        self.assertFalse((self.mod / "assets/cooked/npc-bodies.v1.json").exists())

    def test_commented_manifest_does_not_break_a_creature_only_cook(self):
        self.doc["creatures"] = self.doc.pop("npcs")
        del self.doc["creatures"]["tobin"]["motions"]
        self.write()
        (self.mod / "kiln.mod.json").write_text('{ // an ordinary manifest\n "id": "npc-mod", "clients": "mod", }')
        self.assertTrue(R.Run(self.recipe, TYPES).run(update_manifest=True)["ok"])

    def test_commented_manifest_resolves_same_mod_body_parts(self):
        npc = self.doc["npcs"]["tobin"]
        self.doc["bodies"] = {"rig": npc["body"]}
        npc["body"] = "npc-mod:rig"
        self.write()
        (self.mod / "kiln.mod.json").write_text('{ // authored body\n "id": "npc-mod", "clients": "mod", }')
        self.assertTrue(R.Run(self.recipe, TYPES).run(update_manifest=True)["ok"])

    def test_manifest_id_is_only_needed_for_qualified_parts(self):
        (self.mod / "kiln.mod.json").write_text('[]')
        self.assertIn("tobin", B.npc_bodies(self.mod))
        self.doc["npcs"]["tobin"]["body"] = "npc-mod:rig"
        self.write()
        with self.assertRaisesRegex(B.BlocksError, "kiln.mod.json"):
            B.npc_bodies(self.mod)

    def test_materials_without_graph_metadata_read_a_relaxed_mod_id(self):
        from anicook.graphgen import derive
        R.Run(self.recipe, TYPES).run(update_manifest=True)
        entries = self.stage_model()
        path = self.mod / "assets/cooked/models_golem.model.json"
        model = json.loads(path.read_text())
        model["materials"] = [derive("npc-mod", "material", "skin")]
        path.write_text(json.dumps(model))
        (self.mod / "assets/cooked/tobin.animation.tobin.body.graph.json").unlink()
        (self.mod / "blocks/skin.json").write_text(json.dumps({"materials": {"skin": {"roughness": 0.5}}}))
        (self.mod / "kiln.mod.json").write_text('{ // material id\n "id": "npc-mod", }')
        self.assertEqual(([], [], []), B.body_manifest(self.recipe, entries, True))

    def test_a_malformed_cached_row_is_named_and_the_old_cache_is_kept(self):
        # Review finding: "entries": [{}] passed the object check and then failed with KeyError at the sort.
        self.assertTrue(R.Run(self.recipe, TYPES).run(update_manifest=True)["ok"])
        cache = self.mod / "assets/cooked/npc-bodies.v1.json"
        good = json.loads(cache.read_text())
        for index, bad in enumerate(({}, {"kind": "resource"}, {"kind": "resource", "file": 3, "producer": "modelcook"},
                                     {"kind": "blob", "file": "assets/cooked/x.blob"}, {"kind": 1, "file": "f", "producer": "p"})):
            with self.subTest(bad=bad):
                doc = dict(good, entries=good["entries"] + [bad])
                cache.write_text(json.dumps(doc))
                before = cache.read_bytes()
                row = len(good["entries"])
                with self.assertRaisesRegex(B.BlocksError, rf"npc-bodies\.v1\.entries\[{row}\]: .*kind, file and producer"):
                    B.body_manifest(self.recipe, [], True, producer="modelcook")
                self.assertEqual(before, cache.read_bytes(), "a refused cook leaves the old cache as it was")
                with self.assertRaisesRegex(R.RecipeError, rf"npc-bodies\.v1\.entries\[{row}\]"):
                    R.Run(self.recipe, TYPES).run(update_manifest=True)
                self.assertEqual(before, cache.read_bytes())

    def test_material_admission_records_each_block_identity_not_just_a_shared_JSON_file(self):
        import hashlib
        from anicook.graphgen import derive
        file = self.mod / "blocks/skin.json"
        file.write_text(json.dumps({"format": 1, "materials": {"skin": {"roughness": .5}, "tint": {"was": "stable-tint", "roughness": .7}}}))
        self.assertTrue(R.Run(self.recipe, TYPES).run(update_manifest=True)["ok"])
        entries = self.stage_model()
        side = self.mod / "assets/cooked/models_golem.model.json"
        model = json.loads(side.read_text())
        guids = {"skin": derive("npc-mod", "material", "skin"), "tint": derive("npc-mod", "material", "stable-tint")}
        model["materials"] = list(guids.values())
        side.write_text(json.dumps(model))
        B.body_manifest(self.recipe, entries, True)
        body = json.loads((self.mod / "assets/cooked/npc-bodies.v1.json").read_text())["bodies"]["tobin"]
        self.assertEqual({"skin", "tint"}, {m["name"] for m in body["materialBlocks"]})
        for material in body["materialBlocks"]:
            self.assertEqual("npc-mod", material["mod"])
            self.assertEqual(guids[material["name"]], material["guid"])
            self.assertEqual("blocks/skin.json", material["file"])
            self.assertEqual(hashlib.sha256(file.read_bytes()).hexdigest(), material["sha256"])
        self.assertTrue(body["complete"], "prepared for native admission, not a claim that materials were published or that the body is Ready")

    def test_material_sources_and_textures_remain_in_source_identity_without_renderer_metadata(self):
        import hashlib
        texture = self.mod / "models/skin.png"
        texture.write_bytes(b"synthetic texture source")
        material = self.mod / "blocks/skin.json"
        material.write_text(json.dumps({"format": 1, "materials": {"skin": {"albedo": "models/skin.png"}}}))
        self.assertTrue(R.Run(self.recipe, TYPES).run(update_manifest=True)["ok"])
        cache = self.mod / "assets/cooked/npc-bodies.v1.json"
        before = json.loads(cache.read_text())
        self.assertEqual([], before["bodies"]["tobin"]["materialBlocks"], "no renderer metadata has selected materials")
        sources = {row["file"]: row["sha256"] for row in before["sources"]}
        for file in (texture, material):
            self.assertEqual(hashlib.sha256(file.read_bytes()).hexdigest(), sources[file.relative_to(self.mod).as_posix()])
        texture.write_bytes(b"changed synthetic source")
        self.assertTrue(R.Run(self.recipe, TYPES).run(update_manifest=True)["ok"])
        self.assertNotEqual(before["sourceHash"], json.loads(cache.read_text())["sourceHash"])

    def test_a_shared_creature_rig_is_refused_before_its_manifest_changes(self):
        import copy
        npc = copy.deepcopy(self.doc["npcs"]["tobin"])
        creature = copy.deepcopy(npc)
        del creature["motions"]
        self.doc = {"creatures": {"hostile": creature}}
        self.write()
        self.assertTrue(R.Run(self.recipe, TYPES).run(update_manifest=True)["ok"])
        manifest = self.mod / "assets/assets.json"
        before = manifest.read_bytes()
        self.assertTrue(json.loads(before)["entries"])
        self.doc["npcs"] = {"tobin": npc}
        self.write()
        with self.assertRaisesRegex((B.BlocksError, R.RecipeError), "tobin.*creature hostile"):
            R.Run(self.recipe, TYPES).run(update_manifest=True)
        self.assertEqual(before, manifest.read_bytes())

    def test_an_unowned_animation_on_the_roster_rig_is_refused(self):
        import copy
        self.doc["animations"] = {"spare": copy.deepcopy(self.doc["npcs"]["tobin"]["animation"])}
        self.write()
        with self.assertRaisesRegex((B.BlocksError, R.RecipeError), "tobin.*animation spare"):
            R.Run(self.recipe, TYPES).run(update_manifest=True)

    def test_an_extra_recipe_graph_cannot_lose_its_eager_skeleton_and_clips(self):
        graph = dict(B.plan(self.mod)["graphs"][0], name="legacy_graph")
        steps = R.load(self.recipe)["steps"] + [graph]
        self.recipe.write_text(json.dumps({"format": 1, "tool": "animcook", "steps": steps}))
        with self.assertRaisesRegex((B.BlocksError, R.RecipeError), "tobin.*graph legacy_graph"):
            R.Run(self.recipe, TYPES).run(update_manifest=True)

    def test_a_look_cannot_take_the_roster_model_out_of_its_eager_manifest(self):
        self.doc["looks"] = {"coat": {"model": "models/golem.glb"}}
        self.write()
        with self.assertRaisesRegex((B.BlocksError, R.RecipeError), "tobin.*look coat"):
            R.Run(self.recipe, TYPES).run(update_manifest=True)

    def test_an_extra_model_step_on_the_roster_rig_is_refused(self):
        with self.assertRaisesRegex(B.BlocksError, "tobin.*model other_model"):
            B.body_step(self.recipe, {"op": "model", "name": "other_model", "skeleton": "golem"})

    def test_two_npcs_may_share_one_private_group(self):
        import copy
        self.doc["npcs"]["second"] = copy.deepcopy(self.doc["npcs"]["tobin"])
        self.write()
        self.assertTrue(R.Run(self.recipe, TYPES).run(update_manifest=True)["ok"])
        self.assertEqual([], json.loads((self.mod / "assets/assets.json").read_text())["entries"])
        self.assertEqual({"tobin", "second"}, set(json.loads((self.mod / "assets/cooked/npc-bodies.v1.json").read_text())["bodies"]))

    def test_one_run_resolves_its_body_specs_once_not_per_step(self):
        from unittest.mock import patch
        run = R.Run(self.recipe, TYPES)
        with patch.object(B, "npc_bodies", wraps=B.npc_bodies) as resolve:
            self.assertTrue(run.run(update_manifest=True)["ok"])
        self.assertEqual(1, resolve.call_count)

    def stage_model(self):
        # The model emitter's output is synthetic: the split seam needs no installed game.
        model = self.mod / "assets/cooked/models_golem.model.bin"
        blob = self.mod / "assets/cooked/models_golem.render.blob"
        model.write_bytes(b"model")
        blob.write_bytes(b"blob")
        side = {"guid": "8bb42bf4-b129-511d-b210-f91bff63d45a", "skeleton": json.loads((self.mod / "assets/cooked/tobin.animation.tobin.body.graph.json").read_text())["skeleton"],
                "boundingBox": {"min": {"x": -0.2, "y": 0, "z": -0.2}, "max": {"x": 0.2, "y": 1.8, "z": 0.2}}, "materials": []}
        (self.mod / "assets/cooked/models_golem.model.json").write_text(json.dumps(side))
        from cook.steps import model_entries
        entries = model_entries(side["guid"], "new", "cooked/models_golem.model.bin", "cooked/models_golem.render.blob")
        return entries

    def test_both_cooks_merge_the_same_deferred_group(self):
        R.Run(self.recipe, TYPES).run(update_manifest=True)
        entries = self.stage_model()
        normal, missing, mismatched = B.body_manifest(self.recipe, entries, True)
        self.assertEqual((normal, missing, mismatched), ([], [], []))
        bundle = json.loads((self.mod / "assets/cooked/npc-bodies.v1.json").read_text())
        self.assertTrue(bundle["bodies"]["tobin"]["complete"])
        self.assertTrue(all(e["file"].startswith("assets/") for e in bundle["entries"]))
        before = (self.mod / "assets/cooked/npc-bodies.v1.json").read_bytes()
        B.body_manifest(self.recipe, entries, True)
        self.assertEqual(before, (self.mod / "assets/cooked/npc-bodies.v1.json").read_bytes())

    def test_a_first_cook_whose_renderer_writes_material_blocks_ends_with_one_source_set(self):
        # Found by the 16f S6 lab: on a mod's first cook modelcook (dispatched after animcook's own steps) writes the glTF's material
        # blocks, a body source animcook had not hashed, so animcook's rows belonged to an older source set and the body had no bindings.
        import animcook
        from types import SimpleNamespace
        from unittest.mock import patch
        from cook.steps import model_entries
        material = self.mod / "blocks/models_golem.materials.json"
        calls = []

        def renderer(a, doc, extra):  # what modelcook does on a dedicated server: the material blocks first, then its rows of the cache
            calls.append(extra)
            material.write_text(json.dumps({"format": 1, "materials": {"models_golem.skin": {"roughness": 1}}}))
            entries = model_entries("8bb42bf4-b129-511d-b210-f91bff63d45a", "new", "cooked/models_golem.model.bin", "cooked/models_golem.render.blob")
            B.body_manifest(self.recipe, entries, True, server=True)
            return 0

        args = SimpleNamespace(recipe=str(self.recipe), no_dispatch=False, check=False, update_manifest=True, no_game=True, game=".", exe=None)
        with patch.object(animcook, "_dispatch", side_effect=renderer), patch.object(animcook.recipe, "other_tools", return_value=["modelcook"]), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, animcook.cmd_recipe(args))
        self.assertEqual(1, len(calls), "the renderer's cook runs once")
        bundle = json.loads((self.mod / "assets/cooked/npc-bodies.v1.json").read_text())
        self.assertIn("blocks/models_golem.materials.json", {s["file"] for s in bundle["sources"]})
        self.assertEqual({bundle["sourceHash"]}, {e["sourceHash"] for e in bundle["entries"]}, "both tools' rows belong to the same sources")
        self.assertEqual({"animcook", "modelcook"}, {e["producer"] for e in bundle["entries"]})
        self.assertEqual(27, len(bundle["bodies"]["tobin"]["bindings"]))
        self.assertEqual(bundle["sourceHash"], bundle["bodies"]["tobin"]["sourceHash"])

    def test_missing_renderer_outputs_cannot_reuse_an_authors_complete_body(self):
        R.Run(self.recipe, TYPES).run(update_manifest=True)
        entries = self.stage_model()
        B.body_manifest(self.recipe, entries, True)
        path = self.mod / "assets/cooked/npc-bodies.v1.json"
        source_id = json.loads(path.read_text())["bodies"]["tobin"]["sourceHash"]
        for name in ("models_golem.model.bin", "models_golem.render.blob", "models_golem.model.json"):
            (self.mod / "assets/cooked" / name).unlink()
        self.assertTrue(R.Run(self.recipe, TYPES).run(update_manifest=True)["ok"])
        self.assertFalse(json.loads(path.read_text())["bodies"]["tobin"]["complete"])
        self.assertEqual(([], [], []), B.body_manifest(self.recipe, entries, True, server=True))
        bundle = json.loads(path.read_text())
        model_rows = [e for e in bundle["entries"] if e["producer"] == "modelcook"]
        self.assertTrue(all(e["skippedOnServer"] and e["sha256"] is None for e in model_rows))
        self.assertEqual(source_id, bundle["bodies"]["tobin"]["sourceHash"])
        self.assertFalse(bundle["bodies"]["tobin"]["complete"])

    def test_an_unrelated_cook_cannot_bless_corrupt_model_bytes_by_rehashing_them(self):
        R.Run(self.recipe, TYPES).run(update_manifest=True)
        entries = self.stage_model()
        B.body_manifest(self.recipe, entries, True)
        path = self.mod / "assets/cooked/npc-bodies.v1.json"
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
        bundle = json.loads((self.mod / "assets/cooked/npc-bodies.v1.json").read_text())
        self.assertFalse(bundle["bodies"]["tobin"]["complete"])

    def test_manifest_does_not_follow_a_link(self):
        target = self.mod / "outside.json"
        target.write_text("untouched")
        path = self.mod / "assets/cooked/npc-bodies.v1.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to(target)
        with self.assertRaises((B.BlocksError, R.RecipeError)):
            R.Run(self.recipe, TYPES).run(update_manifest=True)
        self.assertEqual(target.read_text(), "untouched")
