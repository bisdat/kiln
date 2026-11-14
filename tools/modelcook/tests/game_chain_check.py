#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks the chain writer (cook/chains.py) against the bone chains of your installed game. Reads the game, writes nothing; prints counts only.

    python3 tools/modelcook/tests/game_chain_check.py [--game DIR]

For every chains2 chain of every RenderModel: the writer's canonical frame of the chain's stored anchor pose and local matrices, quantized
against the chain's stored extent, against the game's packed rest positions (chapter 16e 2.2: within one saturated code step for all 139 of
the research build). Also the palette rule the writer follows (every chain joint's skinningMap entry is a placeholder 0) and the list
relations the writer keeps. Exit 0 when every chain agrees, 1 otherwise, 2 without a game.
"""
import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent), str(HERE.parent.parent / "kfc"), str(HERE.parent.parent / "animcook")]

from cook import chains as CH  # noqa: E402
from kfc.archive import Kfc  # noqa: E402
from kfc.blob import Decoder  # noqa: E402
from kfc.refl import TypeDb  # noqa: E402

GAME = Path.home() / ".local/share/Steam/steamapps/common/Enshrouded"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=str(GAME))
    a = ap.parse_args(argv)
    game = Path(a.game)
    if not (game / "enshrouded.kfc").exists():
        print(f"game_chain_check: no game client in {game} (enshrouded.kfc): nothing checked")
        return 2
    k, t = Kfc(game), TypeDb.from_exe(game / "enshrouded.exe")
    dec, rm = Decoder(t, max_array=1 << 26), t.get("keen::RenderModel")
    n = {"models": 0, "chains": 0, "within_one_code": 0, "worst_code_error": 0, "palette_placeholders": 0, "lists_agree": 0, "count_2_to_8": 0}
    for key in k.resources_of_type(rm["hash"]):
        raw = k.read(key)
        if int.from_bytes(raw[0xE0:0xE4], "little", signed=True) == 0:  # RenderModel.chain: an empty optional
            continue
        d = dec.decode(rm, raw)
        c = d["chain"]
        if not c or not c["chains2"]:
            continue
        n["models"] += 1
        joints = c["joints"] if isinstance(c["joints"], list) else list(bytes.fromhex(c["joints"]["bytes"]))
        n["palette_placeholders"] += all(d["skinningMap"][j] == 0 for j in joints)
        for i, ch in enumerate(c["chains2"]):
            cfg = ch["config"]
            n["chains"] += 1
            n["count_2_to_8"] += 2 <= cfg["count"] <= 8
            n["lists_agree"] += ch["jointEndIndex"] - ch["jointBeginIndex"] == cfg["count"] and len(cfg["restPosition"]) == cfg["count"] - 1
            local = [CH.matrix_of(m) for m in c["modelMatrices"][ch["jointBeginIndex"]:ch["jointEndIndex"]]]
            rest = CH.canonical(CH.matrix_of(c["anchorDefaultPose"][len(c["chains"]) + i]), local)  # legacy chains own the first anchors (none in the game)
            err = max(abs(a_ - b_) for r, w in zip(rest, cfg["restPosition"]) for a_, b_ in zip(CH.codes(CH.pack(r, cfg["extentRestPosition"])), CH.codes(w)))
            n["worst_code_error"] = max(n["worst_code_error"], err)
            n["within_one_code"] += err <= 1
    k.close()
    print("game_chain_check: " + ", ".join(f"{x} {v}" for x, v in n.items()))
    ok = n["chains"] > 0 and n["within_one_code"] == n["chains"] == n["lists_agree"] == n["count_2_to_8"] and n["palette_placeholders"] == n["models"]
    print("game_chain_check: " + ("PASS every chain re-encodes within one code step" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
