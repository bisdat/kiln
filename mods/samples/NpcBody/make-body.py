#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Makes this sample's body with no game data: a script writes a rigged figure (15 joints, a skinned mesh, Idle, Walk, Run, Attack, Dead, Bend,
Hold, Flinch) as models/figure.glb, then the cook turns the body, animation and motions of blocks/npc-body.json into the game's formats and
assets/cooked/npc-bodies.v1.json. `kiln build` and `kiln cook` run the cook by themselves; the figure stands in for what you would export from
Blender. On a dedicated server, cook with its files (`kiln cook <mod> --game <server folder>`): it makes no model there.

    python3 mods/samples/NpcBody/make-body.py
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent
(HERE / "models").mkdir(exist_ok=True)
subprocess.check_call([sys.executable, str(ROOT / "tools/modelcook/samples/rigged_figure.py"), str(HERE / "models/figure.glb")])
subprocess.check_call([sys.executable, str(ROOT / "tools/animcook/animcook.py"), "recipe", str(HERE / "assets/cook.json"), "--update-manifest"])
