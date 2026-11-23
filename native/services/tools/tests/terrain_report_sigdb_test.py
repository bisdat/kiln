#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only test of the terrain kernel's removal report on explicitly supplied game copies. Never executes either binary.

The tool action trace (kiln#133) reads the report the kernel adds each carved cell to: its pointer in the brush
descriptor and its float total. Both offsets are captured from the kernel's own accumulation on each build
(sigdb/symbols/server.d/tools_terrain.json), never assumed; a capture that disagrees leaves them unresolved.

    python3 -I native/services/tools/tests/terrain_report_sigdb_test.py --server <server exe> --client <client exe>
"""
import argparse
import copy
import json
from pathlib import Path
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools/sigport"))
from peimg import Image
from sides import overlay
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
    base = ROOT / "sigdb/symbols"
    server = json.loads((base / "server.d/tools_terrain.json").read_text())
    client = overlay(server, json.loads((base / "client.d/tools_terrain.json").read_text()),
                     "server.d/tools_terrain.json", "client.d/tools_terrain.json")
    probe = "@tools/TerrainCarveKernel.report"
    for side, path, doc, kernel in [("server", args.server, server, 0xa3a180), ("client", args.client, client, 0xa8f880)]:
        image = Image(path)
        symbols, constants, _ = Resolver(image, doc).run()
        check(symbols["tools/TerrainCarveKernel"].get("rva") == kernel, f"{side} kernel at {kernel:#x}")
        check(symbols.get(probe, {}).get("ok", False), f"{side} {probe} matches once inside the kernel")
        for name, value in [("tools/carve.report", 0x80), ("tools/carve.report_total", 0x18)]:
            r = constants.get(name, {})
            check(r.get("ok") and r.get("value") == value and r.get("source") == "2 captures agree",
                  f"{side} {name} = {value:#x}, both captures agree")
        # The probe is the kernel's own accumulation: report = [descriptor + report], total += carved at report + total.
        spec = doc["symbols"][probe]["find"][0]
        rx, holes, _ = parse(spec["pattern"])
        lo = symbols["tools/TerrainCarveKernel"]["rva"]
        hits = Resolver(image, {}).scan(rx, lo, lo + spec["scope"]["size"])
        check(len(hits) == 1, f"{side} {probe} unique in scope")
        at = hits[0]
        # A disagreeing capture refuses the layout: the trace then reports strokes as unmeasured.
        for name in ("tools/carve.report", "tools/carve.report_total"):
            offset = [o for n, _, o in holes if n == name][1]
            changed = copy.copy(image)
            raw = bytearray(image.img)
            raw[at + offset] ^= 4
            changed.img = bytes(raw)
            r = Resolver(changed, doc).run()[1][name]
            check(not r["ok"], f"{side} a disagreeing {name} capture is refused: {r.get('error', '')}")
    print(f"terrain report sigdb: {checks} passed, 0 failed")


if __name__ == "__main__":
    main()
