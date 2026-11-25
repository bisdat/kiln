# SPDX-License-Identifier: MIT
"""Other mods that find game code by its bytes (sigdb/foreign/*.json), on the pinned executables; never runs a game binary.

A native mod that looks a function up by its first bytes once at startup (Building Tools: 4 s after it loads) misses it
when Kiln has already put a jump there, and that feature of the mod stays off. Each registry file lists such scans for
one mod, with every Kiln symbol on their bytes and what Kiln does there. This test checks, per side: each scan still
finds its one match in the game's code; every Kiln symbol a 5-byte patch at which would touch the scanned bytes is
listed under the scan's "kiln" (a new Kiln symbol there fails until someone says whether Kiln hooks it); and every listed
symbol resolves on those bytes. Listed entries starting with OPEN are printed as known conflicts.

    SIGPORT_SERVER=<enshrouded_server.exe> SIGPORT_CLIENT=<enshrouded.exe> \
        python3 -I tools/sigport/tests/test_foreign_scans.py -v
"""
import json
import os
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from peimg import Image  # noqa: E402
from sides import compose  # noqa: E402
from sigdb_check import Resolver, parse  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
REGISTRY = ROOT / "sigdb" / "foreign"
EXE = {"server": os.environ.get("SIGPORT_SERVER") or os.environ.get("KILN_SERVER_EXE"),
       "client": os.environ.get("SIGPORT_CLIENT") or os.environ.get("KILN_CLIENT_EXE")}
PATCH = 5  # kiln.hooks' jump; a mid hook moves whole instructions, so at least these bytes change


def registry():
    return [(p.name, json.loads(p.read_text())) for p in sorted(REGISTRY.glob("*.json"))]


class ForeignScans(unittest.TestCase):
    def test_registry_shape(self):
        files = registry()
        self.assertTrue(files, "sigdb/foreign has no registry file")
        for name, doc in files:
            self.assertEqual(doc.get("format"), 1, name)
            for key in ("id", "name", "version", "source", "scans"):
                self.assertIn(key, doc, f"{name}: {key}")
            for scan in doc["scans"]:
                self.assertTrue(set(scan["sides"]) <= {"server", "client"} and scan["sides"], f"{name} {scan['name']}: sides")
                self.assertIn(scan["lookup"], ("exact", "jump-tolerant"), f"{name} {scan['name']}: lookup")
                if scan["lookup"] == "jump-tolerant":
                    self.assertGreaterEqual(scan.get("head", 0), 5, f"{name} {scan['name']}: head (the bytes a jump may take)")
                parse(scan["pattern"])
                for symbol, what in scan.get("kiln", {}).items():
                    self.assertTrue(what.strip(), f"{name} {scan['name']}: say what Kiln does at {symbol}")

    def side(self, side):
        path = EXE[side]
        if not path:
            self.skipTest(f"set SIGPORT_{side.upper()} to the pinned {side} executable")
        image = Image(Path(path))
        symbols = {}
        for _, part, _ in compose(side):
            symbols.update(part.get("symbols", {}))
        found, _, _ = Resolver(image, {"symbols": symbols, "features": {}}).run()
        resolved = {n: f["rva"] for n, f in found.items() if f.get("ok") and not n.startswith("@")}
        lo, hi = image.text_va, image.text_va + len(image.text)
        for file, doc in registry():
            for scan in doc["scans"]:
                if side not in scan["sides"]:
                    continue
                what = f"{side} {doc['name']} {doc['version']} {scan['name']}"
                rx, _, size = parse(scan["pattern"])
                hits = Resolver(image, {}).scan(rx, lo, hi)
                self.assertEqual(len(hits), 1, f"{what}: the signature has {len(hits)} matches (one expected): stale for this build?")
                at = hits[0]
                # a jump-tolerant scan accepts another mod's jump in its first `head` bytes; its symbols are listed all the same
                on = sorted(n for n, rva in resolved.items() if rva < at + size and rva + PATCH > at)
                listed = scan.get("kiln", {})
                for n in on:
                    self.assertIn(n, listed, f"{what}: Kiln's {n} ({resolved[n]:#x}) is on the bytes it finds "
                                             f"({at:#x}+{size}); list it in sigdb/foreign/{file} with what Kiln does there")
                for n, note in listed.items():
                    self.assertIn(n, on, f"{what}: {n} is listed but not on its bytes on this build")
                    if note.startswith("OPEN"):
                        print(f"KNOWN {what}: {n}: {note}", flush=True)
                print(f"PASS {what} at {at:#x} (+{size}): " + (", ".join(on) if on else "no Kiln symbol on its bytes"), flush=True)

    def test_server(self):
        self.side("server")

    def test_client(self):
        self.side("client")


if __name__ == "__main__":
    unittest.main()
