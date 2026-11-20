#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds the lab test's files on your machine, with no game data in the repository:

1. models/banner.gltf (the research's reference banner) and models/pendant.gltf (three links on a skeleton of their own), written by
   mods/samples/ModelPhysics/make_sources.py;
2. the cook of assets/cook.json against the installed game, read only, into assets/cooked; its assets.json is moved to
   assets/cooked/manifest.json, which the test loads itself (a dedicated server skips a model's entries at boot);
3. the same mod cooked with a bad pin in its physics.json, in a temporary folder: it has to be refused, by the field, writing nothing. The
   outcome goes to assets/cooked/refused-build.json;
4. assets/cooked/golden.json: what the test compares the game's reflection of the cooked banner and pendant with (the banner's fabric is the
   research's golden, tools/modelcook/tests/fixtures/banner/expected-fabric.json, in float32).

    python3 mods/tests/model-physics-test/build-assets.py   [GAME=<client folder>]
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent
GAME = os.environ.get("GAME", str(Path.home() / ".local/share/Steam/steamapps/common/Enshrouded"))
MODELCOOK = ROOT / "tools/modelcook/modelcook.py"


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def cook(recipe):
    return subprocess.run([sys.executable, str(MODELCOOK), "--game", GAME, "recipe", str(recipe), "--update-manifest"], capture_output=True, text=True)


def main():
    (HERE / "models").mkdir(exist_ok=True)
    subprocess.check_call([sys.executable, str(ROOT / "mods/samples/ModelPhysics/make_sources.py"), str(HERE / "models")], stdout=subprocess.DEVNULL)
    (HERE / "models/cape.gltf").unlink()
    r = cook(HERE / "assets/cook.json")
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode:
        return r.returncode
    cooked = HERE / "assets/cooked"
    shutil.move(str(HERE / "assets/assets.json"), str(cooked / "manifest.json"))

    with tempfile.TemporaryDirectory() as t:  # the same mod with a pin outside [0, 1]
        mod = Path(t) / "model-physics-test"
        for part in ("kiln.mod.json", "models", "blocks", "assets/cook.json"):
            src, dst = HERE / part, mod / part
            dst.parent.mkdir(parents=True, exist_ok=True)
            (shutil.copytree if src.is_dir() else shutil.copy)(src, dst)
        doc = json.loads((HERE / "assets/physics.json").read_text())
        doc["models"]["models/banner.gltf"]["cloth"][0]["pin"][2] = 2
        (mod / "assets/physics.json").write_text(json.dumps(doc))
        off = cook(mod / "assets/cook.json")
        made = sorted(str(p.relative_to(mod)) for p in (mod / "assets").rglob("*") if p.is_file() and p.name not in ("cook.json", "physics.json"))
    (cooked / "refused-build.json").write_text(json.dumps({"exit": off.returncode, "refused": off.returncode == 2, "message": off.stderr.strip()[-600:],
                                                         "files": made}, indent=1) + "\n")
    print(f"build-assets: bad physics: exit {off.returncode}, files {made}")

    report = json.loads((cooked / "cook-report.json").read_text())
    want = json.loads((ROOT / "tools/modelcook/tests/fixtures/banner/expected-fabric.json").read_text())
    pendant_step = next(s for s in report["steps"] if s.get("chains"))
    golden = {
        "outputs": {o["file"]: o["sha256"] for o in report["outputs"]},
        "banner": {"guid": next(s["guid"] for s in report["steps"] if s.get("cloth")),
                   "anchors": want["fabric"]["anchors"], "sets": want["fabric"]["sets"], "indices": want["fabric"]["indices"],
                   "tetherLengths": [f32(x) for x in want["fabric"]["tetherLengths"]], "restValues": [f32(x) for x in want["fabric"]["restValues"]],
                   "inverseMasses": [f32(x) for x in want["inverseMasses"]], "skinnedVertexIndices": [0, 1], "renderSimulationVertexIndices": [0, 1, 2, 3]},
        "pendant": {"guid": pendant_step["guid"], "count": pendant_step["chains"][0]["joints"], "joints": pendant_step["chains"][0]["slots"],
                    "extentRestPosition": pendant_step["chains"][0]["extentMm"], "extentMaxAllowedDist": pendant_step["chains"][0]["maxDistMm"],
                    "chainGuid": pendant_step["chains"][0]["guid"]},
    }
    (cooked / "golden.json").write_text(json.dumps(golden, indent=1) + "\n")
    print("build-assets: wrote assets/cooked (manifest.json, refused-build.json, golden.json)")
    return 0 if off.returncode == 2 and not made else 1


if __name__ == "__main__":
    sys.exit(main())
