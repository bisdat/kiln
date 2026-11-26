# SPDX-License-Identifier: MIT
"""Other mods that find game code by its bytes (sigdb/foreign/*.json), on the pinned executables; never runs a game binary.

A native mod that looks a function up by its first bytes once at startup (Building Tools: 4 s after it loads) misses it
when Kiln has already put a jump there, and that feature of the mod stays off. Each registry file lists such scans for
one mod, with every Kiln symbol on their bytes and what Kiln does there. A scan may also name the bytes the mod checks
after the match ("checks": offset and bytes) and the calls it follows or redirects there ("calls": offsets of E8 rel32);
they count as the scan's bytes. This test checks, per side: each scan still finds its one match in the game's code and
its checks and calls hold; every Kiln symbol a 5-byte patch at which would touch those bytes is listed under the scan's
"kiln" (a new Kiln symbol there fails until someone says whether Kiln hooks it); every listed symbol resolves on them;
and every Kiln hook site the scan names under "kiln_sites" (where Kiln hooks that function instead) resolves inside the
function, after the match, clear of all of them. Listed entries starting with OPEN are printed as known conflicts.

    SIGPORT_SERVER=<enshrouded_server.exe> SIGPORT_CLIENT=<enshrouded.exe> \
        python3 -I tools/sigport/tests/test_foreign_scans.py -v
"""
import json
import os
import struct
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


def chunk_of(image, rva):
    """The .pdata entry (begin, end) whose code holds rva, or None."""
    va, size = image.sections[".pdata"]
    for i in range(size // 12):
        begin, end, _ = struct.unpack_from("<III", image.img, va + 12 * i)
        if begin <= rva < end:
            return begin, end
    return None


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
                for check in scan.get("checks", []):
                    self.assertGreater(int(check["at"], 16), 0, f"{name} {scan['name']}: a check's offset")
                    self.assertTrue(bytes.fromhex(check["bytes"]), f"{name} {scan['name']}: a check's bytes")
                for call in scan.get("calls", []):
                    self.assertGreater(int(call, 16), 0, f"{name} {scan['name']}: a call's offset")
                for site, what in scan.get("kiln_sites", {}).items():
                    self.assertTrue(what.strip(), f"{name} {scan['name']}: say what Kiln hooks at {site}")
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
                # what the mod reads: the signature, then its checks and the calls (E8 rel32) it follows
                spans = [(at, at + size)]
                for check in scan.get("checks", []):
                    want = bytes.fromhex(check["bytes"])
                    off = at + int(check["at"], 16)
                    self.assertEqual(bytes(image.img[off:off + len(want)]), want, f"{what}: its check at + {check['at']}")
                    spans.append((off, off + len(want)))
                for call in scan.get("calls", []):
                    off = at + int(call, 16)
                    self.assertEqual(image.img[off], 0xE8, f"{what}: the call it follows at + {call}")
                    spans.append((off, off + 5))
                touches = lambda rva: any(rva < hi and rva + PATCH > lo for lo, hi in spans)
                # a jump-tolerant scan accepts another mod's jump in its first `head` bytes; its symbols are listed all the same
                on = sorted(n for n, rva in resolved.items() if touches(rva))
                listed = scan.get("kiln", {})
                for n in on:
                    self.assertIn(n, listed, f"{what}: Kiln's {n} ({resolved[n]:#x}) is on the bytes it finds "
                                             f"({at:#x}+{size}); list it in sigdb/foreign/{file} with what Kiln does there")
                for n, note in listed.items():
                    self.assertIn(n, on, f"{what}: {n} is listed but not on its bytes on this build")
                    if note.startswith("OPEN"):
                        print(f"KNOWN {what}: {n}: {note}", flush=True)
                # where Kiln hooks this function instead of on those bytes: inside it, after the match, clear of all of them
                chunk = chunk_of(image, at)
                for site in scan.get("kiln_sites", {}):
                    self.assertIn(site, resolved, f"{what}: Kiln's hook site {site} does not resolve on this build")
                    rva = resolved[site]
                    self.assertGreaterEqual(rva, at + size, f"{what}: {site} ({rva:#x}) is not after the signature")
                    self.assertFalse(touches(rva), f"{what}: {site} ({rva:#x}) touches bytes the mod reads")
                    self.assertEqual(chunk_of(image, rva), chunk, f"{what}: {site} ({rva:#x}) is outside the function it finds")
                    print(f"PASS {what}: Kiln hooks {site} at {rva:#x}, clear of what it reads", flush=True)
                print(f"PASS {what} at {at:#x} (+{size}): " + (", ".join(on) if on else "no Kiln symbol on its bytes"), flush=True)

    def test_server(self):
        self.side("server")

    def test_client(self):
        self.side("client")


if __name__ == "__main__":
    unittest.main()
