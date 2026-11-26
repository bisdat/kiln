# SPDX-License-Identifier: MIT
"""Read-only exact-build checks for kiln.client.crosshair (sigdb feature client.crosshair_trace,
sigdb/symbols/client.d/client-crosshair.json, docs/re/client.md "The gameplay crosshair trace"); these tests never
execute a game binary.

The game's client_cursor program calls one scene trace from the local camera along its forward ray each client step and
keeps the 112-byte result on its stack; Kiln observes the instruction after that call and copies the result. The trace
is a client function only: the dedicated server has no client_cursor.

    KILN_CROSSHAIR_CLIENT_EXE=<read-only copy of enshrouded.exe 26430625> \\
        python3 -I -m unittest tools/sigport/tests/test_client_crosshair.py
"""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from peimg import Image
from sides import merged
from sigdb_check import Resolver, parse

ROOT = Path(__file__).resolve().parents[3]
ENV = "KILN_CROSSHAIR_CLIENT_EXE"
SHA = "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77"  # client 26430625
FRAGMENT = ROOT / "sigdb/symbols/client.d/client-crosshair.json"
# client 26430625: the program client_cursor, its single call of the trace, the instruction after it, the trace
PROGRAM = 0x294620
CALL = 0x295608
OBSERVED = 0x29560D
TRACE = 0x28CAA0
HIT = 0xA90  # the result's frame offset: [rbp + 0xa90], 112 bytes


class CrosshairDefinitions(unittest.TestCase):
    """What needs no exe: the fragment's shape."""

    def test_the_feature_names_every_symbol_and_constant_of_the_fragment(self):
        doc = json.loads(FRAGMENT.read_text())
        self.assertEqual("client", doc["side"])
        feature = doc["features"]["client.crosshair_trace"]
        self.assertEqual(sorted([*doc["symbols"], "client/CrosshairTrace"]), sorted(feature["symbols"]))
        self.assertEqual(["client/crosshair.hit"], feature["constants"])
        self.assertEqual(HIT, doc["constants"]["client/crosshair.hit"]["value"])

    def test_the_server_composition_has_no_crosshair(self):
        doc = merged(ROOT / "sigdb/symbols/server.json")
        self.assertNotIn("client.crosshair_trace", doc.get("features", {}))


class CrosshairSignatures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        exe = os.environ.get(ENV)
        if not exe:
            raise unittest.SkipTest(f"set {ENV} to a read-only copy of enshrouded.exe 26430625")
        cls.exe = Path(exe)
        cls.image = Image(cls.exe)
        cls.resolver = Resolver(cls.image, merged(ROOT / "sigdb/symbols/client.json"))
        cls.syms, cls.consts, cls.features = cls.resolver.run()

    def u32(self, at):
        return struct.unpack_from("<I", self.image.img, at)[0]

    def test_exact_build(self):
        self.assertEqual(SHA, hashlib.sha256(self.exe.read_bytes()).hexdigest())

    def test_the_feature_resolves_at_the_reviewed_places(self):
        self.assertTrue(self.features.get("client.crosshair_trace"))
        self.assertEqual(OBSERVED, self.syms["client/CrosshairObserved"]["rva"])
        self.assertEqual(TRACE, self.syms["client/CrosshairTrace"]["rva"])
        self.assertEqual(HIT, self.consts["client/crosshair.hit"]["value"])
        for name in ("@client/CrosshairTrace.rows", "@client/CrosshairTrace.result", "@client/CrosshairTrace.miss"):
            self.assertTrue(self.syms[name]["ok"], name)

    def test_every_pattern_matches_once_in_its_scope(self):
        doc = json.loads(FRAGMENT.read_text())
        for name, definition in doc["symbols"].items():
            for strategy in definition["find"]:
                lo, hi = self.resolver.text_lo, self.resolver.text_hi
                if "scope" in strategy:
                    lo = self.syms[strategy["scope"]["symbol"]]["rva"]
                    hi = min(hi, lo + strategy["scope"]["size"])
                self.assertEqual(1, len(self.resolver.scan(parse(strategy["pattern"])[0], lo, hi)), name)

    def test_the_observation_follows_the_only_call_of_the_trace(self):
        img = self.image.img
        # the call, then the 7-byte lea r9, [rbp + hit] the mid hook relocates (no rip-relative operand)
        self.assertEqual(0xE8, img[CALL])
        self.assertEqual(TRACE, CALL + 5 + struct.unpack_from("<i", img, CALL + 1)[0])
        self.assertEqual(bytes.fromhex("4c 8d 8d") + struct.pack("<I", HIT), bytes(img[OBSERVED:OBSERVED + 7]))
        # its three arguments: rcx = the result [rbp + hit], rdx = the camera context, r8 = the descriptor
        self.assertEqual(bytes.fromhex("48 8d 8d") + struct.pack("<I", HIT), bytes(img[CALL - 7:CALL]))
        # no other direct call or tail jump reaches the trace (rel32 scan of .text)
        lo, hi = self.resolver.text_lo, self.resolver.text_hi
        sites = [m.start() for m in re.finditer(rb"[\xe8\xe9]", bytes(img[lo:hi]))]
        callers = [lo + s for s in sites if lo + s + 5 + struct.unpack_from("<i", img, lo + s + 1)[0] == TRACE]
        self.assertEqual([CALL], callers)

    def test_the_site_is_in_the_program_registered_as_client_cursor(self):
        img = bytes(self.image.img)
        name = img.find(b"\0client_cursor\0") + 1
        self.assertGreater(name, 0)
        ref = img.find(struct.pack("<QQ", self.image.base + name, len("client_cursor")))
        self.assertGreater(ref, 0)
        self.assertEqual(PROGRAM, struct.unpack_from("<Q", img, ref + 16)[0] - self.image.base)
        # the site's unwind chunk (.pdata) chains back to the program's own entry
        raw = self.exe.read_bytes()
        pe = struct.unpack_from("<I", raw, 0x3C)[0]
        va, size = struct.unpack_from("<II", raw, pe + 24 + 112 + 3 * 8)  # PE32+ data directory 3: exceptions
        rows = [struct.unpack_from("<III", img, va + i) for i in range(0, size - 11, 12)]
        chunk = next(r for r in rows if r[0] <= OBSERVED < r[1])
        for _ in range(8):  # UNW_FLAG_CHAININFO: the parent RUNTIME_FUNCTION follows the unwind codes
            info = chunk[2]
            if not (img[info] >> 3) & 4:
                break
            chunk = struct.unpack_from("<III", img, info + 4 + ((img[info + 2] + 1) & ~1) * 2)
        self.assertEqual(PROGRAM, chunk[0])

    def test_the_trace_writes_the_fields_kiln_reads(self):
        img = self.image.img
        # status 0 and the copy of the nearest row (sorted by the squared distance at row + 0x5c) into the result:
        # +0x08 position (moved 1 cm), +0x14 normal, +0x60 material, +0x61 its flag, +0x64 entity,
        # +0x68 squared distance from the trace's start
        rows = self.syms["@client/CrosshairTrace.rows"]
        result = self.syms["@client/CrosshairTrace.result"]
        self.assertTrue(rows["ok"] and result["ok"])
        # the miss: 31 (0x1f) after zeroing the fields; the degenerate camera: 21 (0x15)
        self.assertEqual(bytes.fromhex("41 c6 06 1f"), bytes(img[0x28D084:0x28D088]))
        self.assertEqual(bytes.fromhex("c6 01 15"), bytes(img[0x28CBD9:0x28CBDC]))

    def test_the_reach_is_the_held_items_clamped_to_7_and_31_metres(self):
        img = self.image.img
        at = 0x28CDD0  # movss xmm6, [item + 0x3ac] (ItemInfo.equipment.maxDistance); maxss 7; minss 31
        self.assertEqual(bytes.fromhex("f3 0f 10 b0 ac 03 00 00"), bytes(img[at:at + 8]))
        lo = 0x28CDE5 + struct.unpack_from("<i", img, 0x28CDDD + 4)[0]
        hi = 0x28CDFD + struct.unpack_from("<i", img, 0x28CDF5 + 4)[0]
        self.assertEqual(7.0, struct.unpack_from("<f", img, lo)[0])
        self.assertEqual(31.0, struct.unpack_from("<f", img, hi)[0])


if __name__ == "__main__":
    unittest.main()
