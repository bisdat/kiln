#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only exact-build qualification; no game code is executed."""
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
from sides import compose, merged
from sigdb_check import Resolver, parse
sys.path.insert(0, str(ROOT / "tools/kfc"))
from kfc.refl import extract

BUILDS = {
    "client": (26430625, "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77",
               (0xe8c9b0, 0xa9aac0, 0x4966e0, 0x4a9cf0, 0x4a7c90, 0x10ab270, 0x10c6170, 0x10cc7d0, 0x10cc750)),
    "server": (26430253, "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2",
               (0x933af0, 0xa453c0, 0x22dc40, 0x2426c0, 0x2405a0, 0x82f840, 0x84adc0, 0x8514b0, 0x851430)),
}
NAMES = ("BuildingConfigFind", "VoxelBoxFillAir", "BlockKernel", "VoxelDamageGet", "VoxelDamageAdd",
         "BaseColumnAcquire", "BaseColumnSeekBrick", "BaseBrickDecode", "BaseColumnRelease")


class TerrainQualificationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.images = {}
        for side, (_, sha, _) in BUILDS.items():
            path = getattr(cls.args, side)
            if hashlib.sha256(Path(path).read_bytes()).hexdigest() != sha:
                raise AssertionError(f"{side}: executable identity mismatch")
            cls.images[side] = Image(path)

    def check_symbols(self, side):
        build, sha, addresses = BUILDS[side]
        doc = next(d for name, d, _ in compose(side) if name == "build-terrain.json")
        resolver = Resolver(self.images[side], doc)
        for name, definition in doc["symbols"].items():
            for strategy in definition["find"]:
                # Deliberately count all matches, not only the first two returned by normal resolution.
                hits = resolver.scan(parse(strategy["pattern"])[0], resolver.text_lo, resolver.text_hi, limit=1000)
                print(f"{side} {build}: {name}: {len(hits)} matches {[hex(h) for h in hits]}", flush=True)
                self.assertEqual(len(hits), 1, f"{side}: {name} anchor is not unique")
        symbols, constants, features = Resolver(self.images[side], merged(ROOT / f"sigdb/symbols/{side}.json")).run()
        record = json.loads((ROOT / f"sigdb/builds/{side}/{sha}.json").read_text())
        self.assertEqual(record["steam_build"], build)
        for name, expected in zip(NAMES, addresses):
            full = "build/" + name
            with self.subTest(side=side, symbol=full):
                self.assertTrue(symbols[full]["ok"], symbols[full])
                self.assertEqual(symbols[full]["rva"], expected)
                self.assertEqual(record["symbols"][full], hex(expected))
                self.assertEqual(record["services"]["build-terrain"]["symbols"][full], "static")
        for name, definition in doc.get("constants", {}).items():
            self.assertTrue(constants[name]["ok"], name)
            self.assertEqual(constants[name]["value"], definition["value"])
            self.assertEqual(record["constants"][name], hex(definition["value"]))
        for name in doc.get("features", {}):
            self.assertTrue(features[name], name)
            self.assertTrue(record["features"][name], name)

    def test_generic_fill_entry_is_ambiguous(self):
        broad = "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 48 89 48 08 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ??"
        doc = {"symbols": {"broad": {"find": [{"pattern": broad}]}}}
        for side in BUILDS:
            resolver = Resolver(self.images[side], doc)
            hits = resolver.scan(parse(broad)[0], resolver.text_lo, resolver.text_hi, limit=100)
            self.assertEqual(len(hits), 3)
            symbols, _, _ = resolver.run()
            self.assertFalse(symbols["broad"]["ok"])
            self.assertIn("2+ matches", symbols["broad"]["error"])

    def test_client_symbols(self):
        self.check_symbols("client")

    def test_server_symbols(self):
        self.check_symbols("server")

    def test_fill_keeps_native_cleanup_enabled(self):
        # Grow controls core geometry, not this unconditional {0,1,0} dispatch flag set.
        flags = bytes.fromhex("66 C7 84 24 50 01 00 00 00 01")
        flags_tail = bytes.fromhex("C6 84 24 52 01 00 00 00")
        for side in BUILDS:
            entry = BUILDS[side][2][1]
            body = self.images[side].img[entry:entry + 0x240]
            self.assertEqual(body[0x12a:0x12a + len(flags)], flags)
            self.assertEqual(body[0x137:0x137 + len(flags_tail)], flags_tail)

    def test_terrain_lookup_disambiguation(self):
        for side in BUILDS:
            image = self.images[side]
            doc = next(d for name, d, _ in compose(side) if name == "tools-presentation.json")
            resolver = Resolver(image, doc)
            symbols, _, _ = resolver.run()
            address = 0xe9f270 if side == "client" else 0x9464b0
            self.assertEqual(symbols["tools/TerrainConfigLookup"]["rva"], address)
            broad = parse("4C 8B 51 38 4D 85 D2 74 3A 45 8B 5A 04 33 C9 4D 85 DB 74 2F 4D 63 0A 45 33 C0 66 0F 1F 44 00 00 45 85 C9 75 04 33 C0 EB 04 4B 8D 04 0A 49 03 C0 38 10 74 ?? 48 FF C1 49 81 C0 B8 00 00 00")[0]
            self.assertEqual(resolver.scan(broad, resolver.text_lo, resolver.text_hi, limit=100), [address - 0x60, address])
            self.assertEqual(image.img[address + 0x43:address + 0x46], bytes.fromhex("33 c0 c3"))
            self.assertEqual(image.img[address - 0x60 + 0x43:address - 0x60 + 0x49], bytes.fromhex("b8 10 00 00 00 c3"))

    def test_baked_reader_layout(self):
        expected = dict(handle_size=0xa0, ready=0x90, data=0x20, loader=0x10, id=0x18,
                        lock=0, lock_mode=8, seek=0x30, uniform=0x80, material=0x68,
                        fill=0x70, compressed=0x50, end=0x60, size=0x88, decode_count=512)
        for side in BUILDS:
            doc = next(d for name, d, _ in compose(side) if name == "build-terrain.json")
            for name, value in expected.items():
                self.assertEqual(doc["constants"]["build/base_column." + name]["value"], value)
            image = self.images[side]
            acquire = BUILDS[side][2][5]
            body = image.img[acquire:acquire + 0x400]
            # Loader/id publication followed by the retained borrowed span and readiness flag.
            self.assertIn(bytes.fromhex("48 89 57 10 44 89 47 18"), body)
            self.assertIn(bytes.fromhex("0F 11 47 20 48 39 77 20"), body)
            self.assertIn(bytes.fromhex("40 88 B7 90 00 00 00"), body)
            release = BUILDS[side][2][8]
            self.assertIn(bytes.fromhex("F0 FF 09 C3"), image.img[release:release + 0x80])
            self.assertEqual(expected["decode_count"], 8 ** 3)

    def test_reflected_metadata(self):
        definition = json.loads((ROOT / "sigdb/symbols/server.d/build-terrain.json").read_text())
        offsets = {name: definition["constants"]["build/block_config." + name]["value"]
                   for name in ("item", "hardness", "loot_rate", "health")}
        self.assertEqual(offsets, dict(item=4, hardness=0x14, loot_rate=0x18, health=0x1c))
        expected_fields = {"hardness": (offsets["hardness"], "keen::HardnessCategory"),
                           "healthPoints": (offsets["health"], "keen::sint32")}
        for side in BUILDS:
            dump = extract(getattr(self.args, side))
            self.assertEqual(dump["exe_sha256"], BUILDS[side][1])
            types = {t["name"]: t for t in dump["types"]}
            for kind, size, item, rate in (("Building", 0xb4, "materialItem", "blocksPerLootItemExchangeRate"),
                                           ("Terrain", 0xb8, "terrainItem", "terrainPerLootItemExchangeRate")):
                record = types[f"keen::{kind}TerraformingConfig"]
                self.assertEqual(record["size"], size)
                fields = {f["name"]: (f["offset"], f["type"]) for f in record["fields"]}
                for name, expected in expected_fields.items():
                    self.assertEqual(fields[name], expected)
                self.assertEqual(fields[item], (4, "keen::ItemReference"))
                self.assertEqual(fields[rate], (0x18, "keen::uint32"))
            enum = {v["name"]: v["value"] for v in types["keen::HardnessCategory"]["values"]}
            self.assertEqual(enum, dict(Soft=0, SlightlyHard=1, ModeratelyHard=2, Hard=3, VeryHard=4, Unbreakable=5))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True)
    parser.add_argument("--server", required=True)
    TerrainQualificationTest.args = parser.parse_args()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
