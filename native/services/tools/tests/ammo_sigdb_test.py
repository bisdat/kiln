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
            "ui/AmmoItemLookup":0xe94470,"ui/AmmoItemMap":0xe86770,
            # GitHub issue 140: the HUD list reveal of the stock-cycle count.
            "ui/AmmoCycle":0x40c320,"ui/AmmoCycleReturn":0x3afb07,"ui/AmmoSelectReturn":0x3afa7a,
            "ui/AmmoEventWishReturn":0x2bce29,"ui/AmmoCycleEventCount":0x2bd19a,"ui/AmmoCycleWatch":0xb35d7d,
            "ui/AmmoListRevealRead":0xbe603f}
CONSTANTS = {"ui/ammo.groups":0x14fd0,"ui/ammo.cycle_count":0x2fe8,"ui/ammo.reveal_time":0x31b48}
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
                self.assertEqual(hits,[rva-strategy.get("offset",0)],name)
                print(f"{name}: one exact-build match at {rva:#x}",flush=True)
        symbols,constants,features=self.resolver.run()
        for name,rva in EXPECTED.items():
            self.assertEqual(symbols[name]["rva"],rva)
        for name,value in CONSTANTS.items():
            self.assertTrue(constants[name]["ok"],name)
            self.assertEqual(constants[name]["value"],value,name)
        self.assertTrue(features["ui.ammo.list"] and features["ui.ammo.pick"] and features["ui.ammo.reveal"])
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

    def pdata_chunk(self,rva):
        va,size=self.image.sections[".pdata"]
        for i in range(size//12):
            begin,end,_=struct.unpack_from('<III',self.image.img,va+12*i)
            if begin<=rva<end: return begin,end
        self.fail(f"{rva:#x} in no function")

    def call_target(self,ret):
        image=self.image.img
        self.assertEqual(image[ret-5],0xe8)
        return ret+struct.unpack_from('<i',image,ret-4)[0]

    def test_hud_reveal_chain(self):
        """GitHub issue 140: how the game's own pick shows the HUD list again, link by link on this build."""
        image=self.image.img
        def at(rva,hexbytes): self.assertEqual(bytes(image[rva:rva+len(bytes.fromhex(hexbytes))]),bytes.fromhex(hexbytes),f"{rva:#x}")
        ts=json.loads((ROOT/f"sigdb/types/client/{SHA}.json").read_text())["types"]
        by={t["qualifiedName"]:t for t in ts}
        def offset(t,n): return next(f["offset"] for f in by[t]["fields"] if f["name"]==n)
        # 1. The user's wishes: ClientPlayerInput.clientOnlyPlayerInput.inventoryStockSelection {isAbsolute, selection}
        #    (+0x5a6/+0x5a7) picks a row directly (0x3afa75) or cycles by it (0x40c320, then 0x3afb02), both in the
        #    input handler 0x3af670 that the client frame calls at 0x87a94d. Both pass the play data's groups.
        self.assertEqual(offset("keen::ecs::ClientPlayerInput","clientOnlyPlayerInput"),0x5a4)
        self.assertEqual(offset("keen::ecs::ClientOnlyPlayerInput","inventoryStockSelection"),2)
        self.assertEqual([f["name"] for f in by["keen::ecs::SelectionInput"]["fields"]],["isAbsolute","selection"])
        for ret in (0x3afb07,0x3afa7a):
            self.assertEqual(self.call_target(ret),0x40c150)
            self.assertEqual(self.pdata_chunk(ret),self.pdata_chunk(0x3af9f9))
        self.assertEqual(self.call_target(0x3afac4),0x40c320)
        at(0x3af9f9,"4180bea605000000");at(0x3afa1d,"410fb69ea7050000");at(0x3afaa6,"450fb686a7050000")
        self.assertEqual(self.call_target(0x87a952),0x3af670)
        # The game's automatic wishes are elsewhere: SelectInventoryStockItemEvent in player_ui, and 0x2b4846.
        self.assertEqual(self.call_target(0x2bce29),0x40c150);self.assertEqual(self.call_target(0x2b484b),0x40c150)
        # 2. The pick writes only inventoryStockCycleAction {version +0x98, item +0x9c} and the pending selector.
        self.assertEqual(offset("keen::ecs::ClientPlayerInputData","inventoryStockCycleAction"),0x98)
        # 3. The host (player_control_action_sequence 0x2dcca0 -> 0x2cb090) compares PlayerInput.fromClient's version
        #    (+0xa0 = 8 + 0x98) with consumedInventoryStockCycleAction (+0x3c) and sends UiInventoryStockCycleEvent
        #    (type 0xE2267831, 16 bytes: playerEntityId +8, selectionResult +12).
        self.assertEqual(offset("keen::ecs::PlayerInput","fromClient"),8)
        self.assertEqual(offset("keen::ecs::ServerConsumedPlayerInput","consumedInventoryStockCycleAction"),0x3c)
        event=by["keen::ecs::UiInventoryStockCycleEvent"]
        self.assertEqual((int(event["hash"],16),event["size"]),(0xE2267831,16))
        self.assertEqual((offset(event["qualifiedName"],"playerEntityId"),offset(event["qualifiedName"],"selectionResult")),(8,12))
        at(0x2cb0f1,"8b83a0000000"+"39413c");at(0x2cb39c,"ba317826e241b810000000")
        self.assertEqual(self.call_target(0x2ddf64),0x2cb090)
        # 4. The client's player_ui (0x2b8430, sys-write G38UiPlayData = rsi from 0x2b8588) iterates its 14 event streams
        #    (the last 0x70 bytes of its 0x3c8-byte table at rbp+0x70); the eighth, UiInventoryStockCycleEvent at
        #    rbp+0x400, adds one to G38UiPlayData+0x2fe8 for the local player. The same rsi passes the groups at +0x14fd0.
        at(0x2b844c,"41b8c8030000488d5570");at(0x2bd0c1,"4c8b8500040000");at(0x2b8588,"488bb520020000")
        self.assertEqual(0x70+0x3c8-14*8+7*8,0x400)
        names=[]
        for i in range(14):
            q=struct.unpack_from('<Q',image,0x1e259a0+16*i)[0]-self.image.base
            names.append(bytes(image[q:image.index(0,q)]).decode())
        self.assertEqual(names[7],"UiInventoryStockCycleEvent");self.assertEqual(names[4],"SelectInventoryStockItemEvent")
        self.assertEqual(struct.unpack_from('<Q',image,0x1e25a90)[0]-self.image.base,0x2b8430)
        self.assertEqual(self.pdata_chunk(0x2bd19a),self.pdata_chunk(0x2bce29))
        # 5. The HUD's watcher (0xb34ce0, rdi = play data, rbx = the UI's data +0x10) stamps the time at +0x31b48 when
        #    the count changes; the list draw 0xbe5150 fades from the latest of its own change time, +0x31b38 and that
        #    one (0.5 s and 1.5 s at 0xbe6083/0xbe6090), and draws the group the play data's active index names.
        self.assertEqual(self.pdata_chunk(0xb35d7d)[0],0xb35089)
        at(0xbe6083,"ba0065cd1d");at(0xbe6090,"48c745e0002f6859")
        at(0xbed039,"0fb687c87b0100");at(0xbed05b,"e8f080ffff")

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
