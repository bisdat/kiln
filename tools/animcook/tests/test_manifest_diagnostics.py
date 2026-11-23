# SPDX-License-Identifier: MIT
"""CLI diagnostics distinguish the shared body cache from the eager manifest."""
import contextlib
import io
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path[:0] = [str(ROOT), str(ROOT.parent / "kfc"), str(ROOT.parent / "modelcook")]
import animcook
import modelcook


class ManifestDiagnostics(unittest.TestCase):
    def test_foreign_cache_errors_name_the_cache_and_the_tool_that_must_rebuild_it(self):
        for cli, foreign in ((animcook, "modelcook"), (modelcook, "animcook")):
            with self.subTest(tool=cli.__name__):
                report = {"manifest": {"missing": [], "mismatched": [{"producer": foreign, "file": "assets/cooked/bad.bin"}]},
                          "steps": [], "outputs": [], "ok": False}
                args = SimpleNamespace(recipe="assets/cook.json", no_dispatch=True, check=False, update_manifest=True)
                output = io.StringIO()
                with contextlib.ExitStack() as stack:
                    stack.enter_context(patch.object(cli.recipe, "load", return_value={}))
                    stack.enter_context(patch.object(cli.recipe, "out_dir"))
                    stack.enter_context(patch.object(cli.recipe, "own_steps", return_value=[{}]))
                    if cli is animcook:
                        stack.enter_context(patch.object(cli, "_types", return_value=(None, None, {})))
                        run = stack.enter_context(patch.object(cli.recipe, "Run"))
                        run.return_value.run.return_value = report
                        stack.enter_context(patch.object(cli, "_recipe_player_states", return_value=0))
                    else:
                        stack.enter_context(patch.object(cli, "_ctx", return_value=(None, SimpleNamespace(dump={}))))
                        stack.enter_context(patch.object(cli.recipe, "run", return_value=report))
                    stack.enter_context(contextlib.redirect_stdout(output))
                    self.assertEqual(1, cli.cmd_recipe(args))
                self.assertIn("assets/cooked/npc-bodies.v1.json", output.getvalue())
                self.assertIn("rerun " + foreign, output.getvalue())
                self.assertNotIn("assets.json does not match", output.getvalue())
