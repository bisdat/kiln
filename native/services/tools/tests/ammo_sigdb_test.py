#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exact client build qualification; reads the image, never launches it."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools/sigport"))
from peimg import Image
from sides import compose
from sigdb_check import Resolver, parse
SHA = "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77"
EXPECTED = {"tools/MaterialSelector":0x40e5d0,"ui/AmmoFinalizer":0x40dfe0,"ui/AmmoPick":0x40c150,
            "ui/AmmoItemLookup":0xe94470,"ui/AmmoItemMap":0xe86770}
class AmmoQualification(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = Image(cls.exe)
        cls.doc = next(d for n,d,_ in compose("client") if n=="tools-presentation.json")
        cls.resolver = Resolver(cls.image, cls.doc)

    def test_exact_identity_and_unique_seams(self):
        self.assertEqual(hashlib.sha256(Path(self.exe).read_bytes()).hexdigest(), SHA)
        for name,rva in EXPECTED.items():
            for strategy in self.doc["symbols"].get(name,{"find":[]})["find"]:
                hits = self.resolver.scan(parse(strategy["pattern"])[0], self.resolver.text_lo, self.resolver.text_hi, limit=100)
                self.assertEqual(hits,[rva],name)
                print(f"{name}: one exact-build match at {rva:#x}",flush=True)
        symbols,_,features=self.resolver.run()
        for name,rva in EXPECTED.items():
            self.assertEqual(symbols[name]["rva"],rva)
        self.assertTrue(features["ui.ammo.list"] and features["ui.ammo.pick"])
        source=(ROOT/"native/services/tools/ammo_runtime.inl").read_text()
        for name,rva in EXPECTED.items():
            if name=="tools/MaterialSelector": continue
            if name in ("ui/AmmoFinalizer","ui/AmmoPick"):
                pattern=source.split(',"'+name+'","',1)[1].split('"',1)[0]
                rx,_,length=parse(pattern)
                self.assertIsNotNone(rx.fullmatch(self.image.img[rva:rva+length]))

    def test_request_convergence_and_no_hidden_call_patch(self):
        image=self.image.img
        expected={0x2b4846,0x2bce24,0x3a9612,0x3ade44,0x3afa75,0x3afb02,0x3afcce}
        sites={r for r in range(self.resolver.text_lo,self.resolver.text_hi-5)
               if image[r]==0xe8 and r+5+struct.unpack_from('<i',image,r+1)[0]==0x40c150}
        self.assertEqual(sites,expected)
        for site in (0x40f046,0x40e3c6):
            self.assertEqual(image[site],0xe8)
            self.assertEqual(site+5+struct.unpack_from('<i',image,site+1)[0],0x40dfe0)
        # Groups0/1/6 write exactly the native wish and the local pending selector identity.
        self.assertEqual(bytes(image[0x40c174:0x40c185]),bytes.fromhex('8b44242889829800000044898a9c000000'))
        self.assertEqual(bytes(image[0x40c198:0x40c1a7]),bytes.fromhex('8b82980000004289841940060000c3'))
        source=(ROOT/"native/services/tools/hooks/presentation.cpp").read_text()
        self.assertEqual(source.count('install(selector_hook,'),1)
        self.assertNotIn('0x40f046',source)

    def test_client_component_and_item_semantics(self):
        ts=json.loads((ROOT/f"sigdb/types/client/{SHA}.json").read_text())["types"]
        by={t["qualifiedName"]:t for t in ts}
        for name,size in (("keen::ecs::InventorySetup",84),("keen::ecs::Inventory",96),("keen::ItemStack",12)):
            self.assertEqual(by[name]["size"],size)
        def offset(t,n): return next(f["offset"] for f in by[t]["fields"] if f["name"]==n)
        self.assertEqual(offset("keen::ecs::InventorySetup","linksCategories"),64)
        self.assertEqual(offset("keen::ecs::ClientPlayerInputData","inventoryStockCycleAction"),152)
        self.assertEqual(offset("keen::ecs::InventoryStockCycleAction","selectedItemId"),4)
        cats={v["name"]:v["value"] for v in by["keen::InventoryCategory"]["values"]}
        self.assertEqual((cats["Generic"],cats["Virtual"]),(4,5))
        # Proven map lookup locations: ItemRegistry+38, keys+28, values+40, occupancy+8, capacity+10.
        self.assertIn(bytes.fromhex('488d4a38'),bytes(self.image.img[0xe94470:0xe9449e]))
        self.assertIn(bytes.fromhex('4d8b5128'),bytes(self.image.img[0xe86770:0xe867e0]))

if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--client",required=True)
    AmmoQualification.exe=p.parse_args().client
    unittest.main(argv=[sys.argv[0]],verbosity=2)
