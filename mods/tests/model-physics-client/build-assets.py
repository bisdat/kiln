#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds the 16e-cc16e fixture's files on your machine, with no game data in the repository:

1. models/banner.gltf (a 1 m banner, 8 x 8 quads, its top edge pinned), models/cape.gltf (a cape cut down its middle for its UVs, worn on the
   player's Chest joint, the joint at the file's origin) and models/pendant.gltf (three links on a skeleton of their own, an Idle clip that
   turns their holder), written by mods/samples/ModelPhysics/make_sources.py;
2. assets/physics.json: the banner's and the cape's cloth (the cape's collider follows the Chest joint, whose index is read from the installed
   game's animation graph); the pendant's chain is its body block's;
3. the cook of assets/cook.json against the installed game, read only;
4. assets/cooked/expected.json: what the writer made, for the check to log and compare: per cloth mesh its particles and pinned particles with
   their bind positions (game space), per chain its joints, extents and GUID.

    python3 mods/tests/model-physics-client/build-assets.py   [GAME=<client folder>]
"""
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent
sys.path[:0] = [str(ROOT / "tools/modelcook"), str(ROOT / "tools/kfc"), str(ROOT / "tools/animcook"), str(ROOT / "mods/samples/ModelPhysics")]
GAME = Path(os.environ.get("GAME", str(Path.home() / ".local/share/Steam/steamapps/common/Enshrouded")))

import make_sources  # noqa: E402
from cook import steps  # noqa: E402
from kfc.archive import Kfc  # noqa: E402
from kfc.blob import Decoder  # noqa: E402
from kfc.refl import TypeDb  # noqa: E402


def main():
    k, t = Kfc(GAME), TypeDb.from_exe(GAME / "enshrouded.exe")
    try:
        chest = steps.attach_joint(k, t, steps.game_target(k, t, steps.PLAYER_SKELETON), "Chest")
    finally:
        k.close()
    models = HERE / "models"
    models.mkdir(exist_ok=True)
    physics = {"format": 1, "models": {
        "models/banner.gltf": make_sources.banner(models / "banner.gltf", subdivisions=8),
        "models/cape.gltf": make_sources.cape(models / "cape.gltf", top=0.05, z=-0.22, collider_joint=f"j{chest}"),
    }}
    make_sources.pendant(models / "pendant.gltf", idle=True)
    physics["models"]["models/banner.gltf"]["cloth"][0]["colliders"] = [{"type": "sphere", "x": 0.0, "y": 0.4, "z": 0.25, "radius": 0.15}]
    (HERE / "assets/physics.json").write_text(json.dumps(physics, indent=1) + "\n")
    r = subprocess.run([sys.executable, str(ROOT / "tools/modelcook/modelcook.py"), "--game", str(GAME), "recipe", str(HERE / "assets/cook.json"),
                        "--update-manifest"])
    if r.returncode:
        return r.returncode

    dec = Decoder(t, max_array=1 << 26)
    rm = t.get("keen::RenderModel")
    report = json.loads((HERE / "assets/cooked/cook-report.json").read_text())
    expected = {"chestJoint": chest, "models": {}}
    for st in report["steps"]:
        if not (st.get("cloth") or st.get("chains")):
            continue
        d = dec.decode(rm, (HERE / "assets" / st["bin"]).read_bytes())
        e = {"guid": st["guid"], "file": st["bin"]}
        if d["cloth"]:
            e["cloth"] = [{"particles": m["data"]["fabric"]["numParticles"], "pinned": m["data"]["skinnedVertexIndices"],
                           "pinPositions": [[round(m["data"]["simulationPositions"][i][a], 4) for a in "xyz"] for i in m["data"]["skinnedVertexIndices"]]}
                          for m in d["cloth"]["meshes"]]
            e["colliders"] = {"spheres": len(d["collider"]["spheres"]), "capsules": len(d["collider"]["capsules"]), "skinned": d["collider"]["hasSkinning"]}
        if d["chain"]:
            e["chains"] = st["chains"]
        expected["models"][Path(st["bin"]).stem] = e
    (HERE / "assets/cooked/expected.json").write_text(json.dumps(expected, indent=1) + "\n")
    print(f"build-assets: Chest is joint {chest}; {len(expected['models'])} models with physics; assets/cooked/expected.json written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
