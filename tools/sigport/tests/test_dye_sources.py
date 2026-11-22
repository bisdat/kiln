# SPDX-License-Identifier: MIT
"""Read-only exact-build checks for Tint; these tests never execute a game binary."""
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from peimg import Image
from sides import merged
from sigdb_check import Resolver, parse

ROOT = Path(__file__).resolve().parents[3]
BUILDS = {
    "server": ("KILN_DYE_SERVER_EXE", "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2",
               {"TintAction": 0x1a1230, "TintPayRet": 0x1a13e8, "TintRollbackRet": 0x1a1455,
                "TintCommitRet": 0x1a1490, "SlotInfo": 0x1445d0}),
    "client": ("KILN_DYE_CLIENT_EXE", "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77",
               {"TintAction": 0x3fff50, "TintPayRet": 0x400108, "TintRollbackRet": 0x400175,
                "TintCommitRet": 0x4001b0, "SlotInfo": 0x3854f0}),
}


class DyeSignatures(unittest.TestCase):
    def test_both_exact_builds_and_unique_shared_patterns(self):
        definitions = json.loads((ROOT / "sigdb/symbols/server.d/dye-sources.json").read_text())
        for side, (env, sha, expected) in BUILDS.items():
            with self.subTest(side=side):
                if not os.environ.get(env):
                    self.skipTest("set both KILN_DYE_SERVER_EXE and KILN_DYE_CLIENT_EXE")
                exe = Path(os.environ[env])
                self.assertEqual(sha, hashlib.sha256(exe.read_bytes()).hexdigest())
                image = Image(exe)
                resolver = Resolver(image, merged(ROOT / f"sigdb/symbols/{side}.json"))
                syms, consts, _ = resolver.run()
                for name, rva in expected.items():
                    self.assertTrue(syms["dyes/" + name]["ok"], name)
                    self.assertEqual(rva, syms["dyes/" + name]["rva"], name)
                for name, defn in definitions["symbols"].items():
                    for strategy in defn["find"]:
                        lo, hi = resolver.text_lo, resolver.text_hi
                        if "scope" in strategy:
                            lo = syms[strategy["scope"]["symbol"]]["rva"]
                            hi = min(hi, lo + strategy["scope"]["size"])
                        self.assertEqual(1, len(resolver.scan(parse(strategy["pattern"])[0], lo, hi)), name)
                for site, target in (("TintPayRet", "items/InventoryRemoveItems"),
                                     ("TintRollbackRet", "items/InventoryRollback"),
                                     ("TintCommitRet", "items/InventoryCommit")):
                    at = syms["dyes/" + site]["rva"] - 5
                    self.assertEqual(0xe8, image.img[at])
                    self.assertEqual(syms[target]["rva"], at + 5 + struct.unpack_from("<i", image.img, at + 1)[0])
                self.assertEqual(8, consts["dyes/action.secondary"]["value"])
                self.assertEqual(0x748, consts["dyes/item.palette_set"]["value"])


if __name__ == "__main__":
    unittest.main()
