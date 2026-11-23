#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exact-build, read-only material lookup signature checks; never executes game code."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools/sigport"))
from peimg import Image
from sides import compose
from sigdb_check import Resolver, parse

SYMBOL = "tools/TerrainConfigLookup"
BUILDS = {
    "client": (26430625, "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77", 0xe9f270),
    "server": (26430253, "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2", 0x9464b0),
}


class MaterialLookupSignatureTest(unittest.TestCase):
    def check_build(self, side):
        path = getattr(self.args, side)
        build, sha, expected = BUILDS[side]
        self.assertEqual(hashlib.sha256(Path(path).read_bytes()).hexdigest(), sha)
        record = json.loads((ROOT / f"sigdb/builds/{side}/{sha}.json").read_text())
        self.assertEqual(record["sha256"], sha)
        self.assertEqual(record["steam_build"], build)
        doc = next(doc for name, doc, _ in compose(side) if name == "tools-presentation.json")
        image = Image(path)
        resolver = Resolver(image, doc)
        for strategy in doc["symbols"][SYMBOL]["find"]:
            hits = resolver.scan(parse(strategy["pattern"])[0], resolver.text_lo, resolver.text_hi, limit=100)
            print(f"{side} {build}: {len(hits)} matches at {[hex(hit) for hit in hits]}", flush=True)
            self.assertEqual(len(hits), 1, f"{side}: lookup anchor must be unique")
            self.assertEqual(hits[0] + strategy.get("offset", 0), expected)
        symbols, _, features = resolver.run()
        self.assertTrue(symbols[SYMBOL]["ok"], symbols[SYMBOL])
        self.assertEqual(symbols[SYMBOL]["rva"], expected)
        self.assertTrue(features["tools.material_lookup"])
        self.assertEqual(record["symbols"][SYMBOL], hex(expected))
        self.assertTrue(record["features"]["tools.material_lookup"])
        self.assertNotEqual(record["services"]["tools-presentation"]["symbols"][SYMBOL], "unresolved")
        # The original-entry guard remains deliberately independent of the discovery signature.
        # Its old prefix also matches the preceding value-returning helper; the new signature
        # extends through the pointer-or-null return, not that helper's field/default return.
        source = (ROOT / "native/services/tools/hooks/presentation.cpp").read_text()
        entry_pattern = source.split('install(lookup_hook,"' + SYMBOL + '","', 1)[1].split('"', 1)[0]
        rx, _, length = parse(entry_pattern)
        self.assertIsNotNone(rx.fullmatch(image.img[expected:expected + length]))
        old_hits = resolver.scan(rx, resolver.text_lo, resolver.text_hi, limit=100)
        self.assertEqual(old_hits, [expected - 0x60, expected])
        print(f"{side} {build}: original prefix has {len(old_hits)} matches; qualified entry {expected:#x}", flush=True)

    def test_client(self):
        self.check_build("client")

    def test_server(self):
        self.check_build("server")

    def test_client_selector_and_brush_frame(self):
        image = Image(self.args.client)
        doc = next(doc for name, doc, _ in compose("client") if name == "tools-presentation.json")
        resolver = Resolver(image, doc)
        expected = {"tools/MaterialSelector": 0x40e5d0, "tools/MaterialPreview": 0xb1a230,
                    "tools/RootIdentity": 0xd3e130, "tools/BrushQueryFrame": 0xc3fe18, "tools/BrushQuery": 0xd25b50}
        for symbol, rva in expected.items():
            for strategy in doc["symbols"][symbol]["find"]:
                hits = resolver.scan(parse(strategy["pattern"])[0], resolver.text_lo, resolver.text_hi, limit=100)
                self.assertEqual([hit + strategy.get("offset", 0) for hit in hits], [rva], symbol)
                print(f"client 26430625: {symbol} has one match at {rva:#x}", flush=True)
        core = json.loads((ROOT / "sigdb/symbols/client.json").read_text())
        core_resolver = Resolver(image, core)
        for strategy in core["symbols"]["GetComponent"]["find"]:
            hits = core_resolver.scan(parse(strategy["pattern"])[0], core_resolver.text_lo, core_resolver.text_hi, limit=100)
            self.assertEqual(hits, [0x9b2ec0])
            print("client 26430625: GetComponent has one match at 0x9b2ec0", flush=True)
        site = expected["tools/BrushQueryFrame"]
        self.assertEqual(image.img[site], 0xe8)
        relative = int.from_bytes(image.img[site+1:site+5], "little", signed=True)
        self.assertEqual(site+5+relative, expected["tools/BrushQuery"])
        # Source of the two original GUID temporaries, independently decoded from the shipped Small brush.
        self.assertEqual(image.img[0x1d8aa68:0x1d8aa78].hex(), "cfa6ae08e8e06c4d8a29a21f4693e006")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True)
    parser.add_argument("--server", required=True)
    MaterialLookupSignatureTest.args = parser.parse_args()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
