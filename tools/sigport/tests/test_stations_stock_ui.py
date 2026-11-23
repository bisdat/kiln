# SPDX-License-Identifier: MIT
"""Read-only exact-build checks for the ingredient rows' "available" count (feature stations.stock_ui); these tests
never execute a game binary.

The count is UiAvailableCount(item, inventory view, stock map). Kiln raises it at the production station's row widget
(two calls) and, since GitHub issue 132, in the crafting menu: the ingredient list's own call (which adds the game's
count of the view's fixed slot table right after) and the per-option call of the menu's ingredient row. These are game
UI code, so they exist only in the client build; the dedicated server has none of them.

    KILN_STOCK_UI_CLIENT_EXE=<enshrouded.exe> KILN_STOCK_UI_SERVER_EXE=<enshrouded_server.exe> \\
        python3 -I -m unittest tools/sigport/tests/test_stations_stock_ui.py
"""
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
CLIENT = ("KILN_STOCK_UI_CLIENT_EXE", "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77")
SERVER = ("KILN_STOCK_UI_SERVER_EXE", "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2")
# client 26430625: the count, the factory row widget's two returns, the crafting menu's two returns, the fixed slots
SITES = {
    "stations/client.UiAvailableCount": 0xd397d0,
    "stations/client.AvailableRowRet1": 0xbb19b8,
    "stations/client.AvailableRowRet2": 0xbb280a,
    "stations/client.AvailableListRet": 0xbf5095,
    "stations/client.AvailableOptionRet": 0xbf352c,
    "stations/client.UiFixedSlotsCount": 0xd39430,
}
RETURNS = ("stations/client.AvailableRowRet1", "stations/client.AvailableRowRet2", "stations/client.AvailableListRet",
           "stations/client.AvailableOptionRet")


def image_of(env, sha, test):
    path = os.environ.get(env)
    if not path:
        test.skipTest(f"set {env}")
    test.assertEqual(sha, hashlib.sha256(Path(path).read_bytes()).hexdigest(), f"{env} is not the qualified build")
    return Image(path)


def call_target(image, ret):
    """The rel32 call that ends at `ret` (its return address): its target."""
    call = ret - 5
    assert image.img[call] == 0xE8, hex(call)
    return ret + struct.unpack_from("<i", image.img, call + 1)[0]


class StockUiSignatures(unittest.TestCase):
    def test_client_sites_resolve_once_each_at_the_qualified_addresses(self):
        image = image_of(*CLIENT, self)
        resolver = Resolver(image, merged(ROOT / "sigdb/symbols/client.json"))
        syms, _, features = resolver.run()
        self.assertTrue(features.get("stations.stock_ui"))
        for name, rva in SITES.items():
            self.assertTrue(syms[name]["ok"], (name, syms[name]))
            self.assertEqual(rva, syms[name]["rva"], name)
        definitions = json.loads((ROOT / "sigdb/symbols/client.d/stations-fill-ui.json").read_text())
        for name, definition in definitions["symbols"].items():
            for strategy in definition["find"]:
                self.assertEqual(1, len(resolver.scan(parse(strategy["pattern"])[0], resolver.text_lo, resolver.text_hi)), name)
        self.assertEqual(set(SITES), set(definitions["features"]["stations.stock_ui"]["symbols"]))

    def test_every_return_follows_a_call_of_the_count(self):
        image = image_of(*CLIENT, self)
        for name in RETURNS:
            self.assertEqual(SITES["stations/client.UiAvailableCount"], call_target(image, SITES[name]), name)

    def test_the_list_adds_the_fixed_slots_of_the_same_view_and_item_after_its_count(self):
        # bf5090 call count(ecx=ebx item, rdx=rsi view, r8 map) / bf5095 mov edi,eax / ... / bf50dc mov rdx,rsi;
        # mov ecx,ebx; call fixed / ... / bf50ee add edi,eax: the row shows count + fixed
        image = image_of(*CLIENT, self)
        ret = SITES["stations/client.AvailableListRet"]
        self.assertEqual(bytes.fromhex("48 8B D6 8B CB"), image.img[ret - 10:ret - 5])
        self.assertEqual(bytes.fromhex("8B F8"), image.img[ret:ret + 2])
        fixed_call = 0xbf50e1
        self.assertEqual(bytes.fromhex("48 8B D6 8B CB E8"), image.img[fixed_call - 5:fixed_call + 1])
        self.assertEqual(SITES["stations/client.UiFixedSlotsCount"], call_target(image, fixed_call + 5))
        self.assertEqual(bytes.fromhex("03 F8"), image.img[0xbf50ee:0xbf50f0])
        # the fixed slot table: +0x290 present, 24 view indices at +0x230, 72-byte views at +0x2d8 (count +0x2e0)
        fixed = SITES["stations/client.UiFixedSlotsCount"]
        self.assertEqual(bytes.fromhex("80 BA 90 02 00 00 00"), image.img[fixed + 0x1c:fixed + 0x23])
        self.assertEqual(bytes.fromhex("0F 10 82 30 02 00 00"), image.img[fixed + 0x29:fixed + 0x30])

    def test_the_menu_sites_read_the_bag_and_base_stock_map_behind_the_lists_flag(self):
        image = image_of(*CLIENT, self)
        # the list: cmp byte [r12+0x25],0 ... add r8,0x2000 (G38UiPlayData+0x2000, the map the factory rows pass)
        self.assertEqual(bytes.fromhex("41 80 7C 24 25 00"), image.img[0xbf5059:0xbf505f])
        self.assertEqual(bytes.fromhex("49 81 C0 00 20 00 00"), image.img[0xbf5084:0xbf508b])
        # the row's options: cmp byte [rdx+0x25],0 ... add r8,0x2000
        self.assertEqual(bytes.fromhex("80 7A 25 00"), image.img[0xbf34ff:0xbf3503])
        self.assertEqual(bytes.fromhex("49 81 C0 00 20 00 00"), image.img[0xbf351e:0xbf3525])

    def test_the_server_build_has_none_of_the_client_ui_sites(self):
        image = image_of(*SERVER, self)
        definitions = json.loads((ROOT / "sigdb/symbols/client.d/stations-fill-ui.json").read_text())
        resolver = Resolver(image, merged(ROOT / "sigdb/symbols/server.json"))
        for name, definition in definitions["symbols"].items():
            for strategy in definition["find"]:
                self.assertEqual(0, len(resolver.scan(parse(strategy["pattern"])[0], resolver.text_lo, resolver.text_hi)), name)


if __name__ == "__main__":
    unittest.main()
