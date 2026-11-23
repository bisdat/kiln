#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only tests of tool payment seams on explicitly supplied game copies. Never executes either binary."""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools/sigport"))
from peimg import Image
from sigdb_check import Resolver, parse

checks = 0

def check(ok, message):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(message)
    print("PASS " + message, flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    args = parser.parse_args()
    doc = json.loads((ROOT / "sigdb/symbols/server.d/tool-payment.json").read_text())
    for side, path, expected in [
        ("server", args.server, [0x228e80, 0x229500, 0x22e630]),
        ("client", args.client, [0x491960, 0x491fe0, 0x4970d0]),
    ]:
        image = Image(path)
        symbols, constants, features = Resolver(image, doc).run()
        check(features["tools.paid_terrain"], side + " complete cost feature resolves")
        for name, address in zip(["tools/TerrainCostBuild", "tools/TerrainCostScale", "tools/MaterialListAppend"], expected):
            check(symbols[name]["rva"] == address, f"{side} {name} exact entry {address:#x}")
        check(constants["tools/cost.cell"]["value"] == 0x28, side + " independent cell captures agree")
        # Both direct cost callers belong to the qualified affordability wrapper (initial and reduced-volume rebuild).
        target, wrapper = expected[:2]
        calls = []
        start, end = image.text_va, image.text_va + len(image.text)
        pos = start
        while True:
            pos = image.img.find(b"\xe8", pos, end - 4)
            if pos < 0:
                break
            if pos + 5 + struct.unpack_from("<i", image.img, pos + 1)[0] == target:
                calls.append(pos)
            pos += 1
        check(calls == [wrapper + 0x34, wrapper + 0x8d], side + " builder has exactly initial and reduced-volume callers")
        # A changed layout or a missing rebuild must lower the whole capability, never silently use vanilla costs.
        for probe in ["@tools/TerrainCostBuild.list", "@tools/MaterialListAppend.layout", "@tools/TerrainCostScale.rebuild"]:
            spec = doc["symbols"][probe]["find"][0]
            lo = symbols[spec["scope"]["symbol"]]["rva"]
            rx, _, _ = parse(spec["pattern"])
            hits = Resolver(image, {}).scan(rx, lo, lo + spec["scope"]["size"])
            check(len(hits) == 1, f"{side} {probe} unique in scope")
            changed = copy.copy(image)
            raw = bytearray(image.img)
            raw[hits[0]] ^= 1
            changed.img = bytes(raw)
            check(not Resolver(changed, doc).run()[2]["tools.paid_terrain"], f"{side} damaged {probe} refuses")
        spec = doc["symbols"]["@tools/TerrainCostBuild.list"]["find"][0]
        rx, holes, _ = parse(spec["pattern"])
        lo = symbols["tools/TerrainCostBuild"]["rva"]
        at = Resolver(image, {}).scan(rx, lo, lo + 164)[0]
        changed = copy.copy(image)
        raw = bytearray(image.img)
        offset = next(offset for name, _, offset in holes if name == "tools/cost.cell")
        raw[at + offset] = 0x29
        changed.img = bytes(raw)
        check(not Resolver(changed, doc).run()[2]["tools.paid_terrain"], side + " disagreeing cell layout refuses")
    print(f"tool payment sigdb: {checks} passed, 0 failed")

if __name__ == "__main__":
    main()
