# SPDX-License-Identifier: MIT
"""Machine-local body manifests must not change a shipped mod's identity."""
import json
import shutil
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

import test_npc_bodies as F
from anicook import cookpaths as CP
from cook import recipe as M, steps as S


class NpcBodyIdentity(unittest.TestCase):
    tearDown = F.NpcBodies.tearDown
    write = F.NpcBodies.write

    def setUp(self):
        F.NpcBodies.setUp(self)
        self.game_tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.game_tmp.cleanup)
        self.game_root = Path(self.game_tmp.name)
        # The authored sample has no glTF materials. Give its explicit model step a synthetic material rather than
        # asking the material-block generator to invent one; the renderer itself remains the only mocked producer.
        self.recipe.write_text(json.dumps({"format": 1, "tool": "animcook", "steps": [{
            "tool": "modelcook", "op": "model", "gltf": "../models/golem.glb", "skeleton": "golem",
            "name": "models/golem", "stem": "models_golem", "mode": "new",
            "material": "177f20db-2990-4f7c-8e15-5df282d16280"}]}))

    def cook(self, root, side="client", model_bytes=b"author model"):
        recipe = root / "assets/cook.json"
        game_dir = self.game_root / side
        (game_dir / "kiln").mkdir(parents=True, exist_ok=True)
        game = types.SimpleNamespace(path=game_dir / ("enshrouded_server.kfc" if side == "server" else "enshrouded.kfc"), version=1)

        def model(*args, **kw):
            stem, out, guid = kw["stem"], kw["subdir"], kw["guid"]
            fb, fo = f"{out}/{stem}.bin", f"{out}/{stem}.blob"
            meta = {"guid": guid, "skeleton": kw["target"].guid, "materials": [],
                    "boundingBox": {"min": {"x": -0.2, "y": 0, "z": -0.2}, "max": {"x": 0.2, "y": 1.8, "z": 0.2}}}
            files = {fb: model_bytes, fo: b"synthetic render data", f"{out}/{stem}.model.json": json.dumps(meta).encode()}
            return files, S.model_entries(guid, "new", fb, fo), {"ok": True, "op": "model"}

        # This is the launcher's actual order. Only the renderer's output bytes are synthetic; both production runners,
        # entry construction, manifest splitting, graph/clip cooking and source fingerprints run normally.
        with patch.object(S, "model", side_effect=model):
            report = M.run(recipe, game, F.TYPES, update_manifest=True)
        self.assertTrue(report["ok"], report)
        self.assertTrue(F.R.Run(recipe, F.TYPES, kfc=game).run(update_manifest=True)["ok"])
        return json.loads(CP.npc_manifest(recipe).read_text())

    @staticmethod
    def identity_files(root):
        return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*")
                if p.is_file() and p.relative_to(root).parts[:2] != ("assets", "cooked")}

    def test_shipped_server_and_differently_cooked_client_have_identical_identity_inputs(self):
        self.cook(self.mod)
        authored = self.identity_files(self.mod)
        with tempfile.TemporaryDirectory() as base:
            copies = {}
            for side in ("server", "client"):
                root = Path(base) / side
                shutil.copytree(self.mod, root, ignore=lambda path, names: ["cooked"] if Path(path).name == "assets" else [])
                copies[side] = self.cook(root, side, b"a newer renderer made different bytes")
                self.assertEqual(authored, self.identity_files(root), f"{side} changed the identity-counted authored files")
            server_id = copies["server"]["bodies"]["tobin"]["sourceHash"]
            client_id = copies["client"]["bodies"]["tobin"]["sourceHash"]
            self.assertEqual(64, len(server_id))
            self.assertEqual(server_id, client_id)

    def test_server_model_rows_are_skipped_not_borrowed_from_the_author(self):
        self.cook(self.mod)
        with tempfile.TemporaryDirectory() as base:
            root = Path(base) / "server"
            shutil.copytree(self.mod, root, ignore=lambda path, names: ["cooked"] if Path(path).name == "assets" else [])
            bundle = self.cook(root, "server")
            rows = [e for e in bundle["entries"] if e["producer"] == "modelcook"]
            self.assertTrue(rows)
            self.assertTrue(all(e.get("skippedOnServer") and e.get("sha256") is None for e in rows))
            self.assertFalse(bundle["bodies"]["tobin"]["complete"])

    def test_one_shared_receipt_proves_the_final_two_producer_manifest(self):
        import hashlib
        import os
        with patch.dict(os.environ, {"KILN_COOK_RECEIPT": "1"}):
            self.cook(self.mod)
        records = [json.loads(p.read_text()) for p in (self.game_root / "client/kiln/state/cook-receipts").glob("*.json")]
        hashes = [r["files"]["cooked/npc-bodies.v1.json"] for r in records if "cooked/npc-bodies.v1.json" in r["files"]]
        self.assertEqual([hashlib.sha256(CP.npc_manifest(self.recipe).read_bytes()).hexdigest()], hashes)

    def test_a_mod_reached_through_a_link_keeps_mod_relative_paths(self):
        with tempfile.TemporaryDirectory() as base:
            linked = Path(base) / "mod"
            linked.symlink_to(self.mod, target_is_directory=True)
            bundle = self.cook(linked)
            self.assertTrue(all(e["file"].startswith("assets/cooked/") for e in bundle["entries"]))
