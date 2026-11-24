#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""#132: qualify the client-only, always-rolled-back craft call and its status/material branches on static copies."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/sigport'))
from peimg import Image
from sides import compose
from sigdb_check import Resolver

checks = 0

def check(ok, text):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(text)
    print('PASS ' + text, flush=True)


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--client', required=True)
    args.add_argument('--server', required=True)
    args = args.parse_args()
    client = Path(args.client)
    server = Path(args.server)
    check(hashlib.sha256(client.read_bytes()).hexdigest() ==
          '5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77', 'qualified client identity')
    check(hashlib.sha256(server.read_bytes()).hexdigest() ==
          'f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2', 'qualified dedicated identity')
    parts = {name: doc for name, doc, _ in compose('client')}
    entry = 'stations/client.CraftPreviewReturn'
    symbols = {entry: parts['stations-craft-ui.json']['symbols'][entry]}
    for name in ['rules/RecipePaid', 'rules/client.CraftableAmount']:
        symbols[name] = parts['rules.json']['symbols'][name]
    names = [entry, 'items/RecipeApply', 'items/InventoryRollback', 'rules/RecipePaid', 'rules/InventoryPay']
    doc = {'symbols': symbols, 'features': {'preview': {'symbols': names}}}
    c = Image(client)
    s = Image(server)
    found, _, features = Resolver(c, doc).run()
    check(features['preview'], 'complete scoped client preview resolves')
    for name, rva in zip(names, [0x2be03f, 0x3f8f30, 0x418150, 0x3f8fec, 0x3f89e0]):
        check(found[name]['rva'] == rva, f'{name} = {rva:#x}')
    check(not Resolver(s, doc).run()[2]['preview'], 'client dry-run feature absent on dedicated executable')
    raw = c.img
    rel = lambda at: at + 5 + struct.unpack_from('<i', raw, at + 1)[0]
    check(rel(0x2be03a) == 0x3f8f30 and rel(0x2be048) == 0x418150,
          'exact craft query calls RecipeApply then rollback')
    check(raw[0x2be03f:0x2be048] == bytes.fromhex('488D8D809C00008BF8'),
          'rollback is unconditional and the recipe result is preserved')
    check(raw[0x411330:0x411336] == bytes.fromhex('40534883EC30'), 'observed PayMaterial is the native per-item wrapper')
    check(raw[0x3f8c42:0x3f8c49] == bytes.fromhex('B804000000EB02'),
          'client InventoryPay exhausted-material branch returns 4')
    check(s.img[0x199ee2:0x199ee9] == bytes.fromhex('B804000000EB02'),
          'dedicated InventoryPay has the corresponding material-only return 4')
    check(raw[0x3f8f68:0x3f8f6d] == bytes.fromhex('B804000000') and
          s.img[0x19a208:0x19a20d] == bytes.fromhex('B804000000'),
          'separate pre-payment eligibility failures also return 4: never raise the final status blindly')
    check(raw[0x2be854:0x2be85b] == bytes.fromhex('C7040F04000000'), 'material failure is cached as STATUS 4')
    check(raw[0x2bea65:0x2bea6c] == bytes.fromhex('C7040701000000'), 'successful full dry run is cached as STATUS 1')
    check(raw[0xd13192:0xd13196] == bytes.fromhex('837D0801') and
          raw[0xd1327f:0xd13283] == bytes.fromhex('837D0801'),
          'craft-one and craft-ten handlers both require STATUS 1, not a count')
    for at in [0x2be03a, 0x2be048]:
        altered = copy.copy(c)
        b = bytearray(raw)
        b[at] = 0x90
        altered.img = bytes(b)
        check(not Resolver(altered, doc).run()[2]['preview'], f'changed call at {at:#x} disables preview')
    check(entry not in json.loads((ROOT / 'sigdb/symbols/server.d/stations.json').read_text())['symbols'],
          'no dedicated preview hook declared')
    print(f'craft preview sigdb: {checks} passed, 0 failed')


if __name__ == '__main__':
    main()
