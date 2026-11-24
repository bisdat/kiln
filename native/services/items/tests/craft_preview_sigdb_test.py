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


def unwind_covers(path, start, size):
    """Include all split runtime-function fragments, not merely the first prologue's pdata entry."""
    raw = path.read_bytes()
    pe = struct.unpack_from('<I', raw, 0x3c)[0]
    section = pe + 24 + struct.unpack_from('<H', raw, pe + 20)[0]
    for n in range(struct.unpack_from('<H', raw, pe + 6)[0]):
        at = section + n * 40
        if raw[at:at + 8].rstrip(b'\0') != b'.pdata':
            continue
        length, offset = struct.unpack_from('<II', raw, at + 16)
        end = start
        for at in range(offset, offset + length - 11, 12):
            lo, hi, _ = struct.unpack_from('<III', raw, at)
            if start <= lo < start + size:
                if lo != end:
                    return False
                end = hi
        return end == start + size
    return False


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
    # Shared read dependency declarations, independently resolved on both pinned executables.
    for side, path, image, expected in [
        ('client', client, c, [0x411360, 0x416400, 0x41154b, 0x411474, 0x46fa10, 0x418150]),
        ('server', server, s, [0x1b0d90, 0x1b5fa0, 0x1b0f7b, 0x1b0ea4, 0x2081c0, 0x1b8310]),
    ]:
        defs = {name: doc for name, doc, _ in compose(side)}
        dep_symbols = copy.deepcopy(defs['craft-preview.json']['symbols'])
        for name in ['items/InventoryRemoveItems', 'items/InventoryRollback']:
            dep_symbols[name] = defs['items.json']['symbols'][name]
        dep_symbols['stations/BaseStockForPlayer'] = defs['stations.json']['symbols']['stations/BaseStockForPlayer']
        dep_names = ['items/InventoryRemoveItems', 'items/InventoryPageFetch', 'stations/MaterialPageFetchRet',
                     'stations/MaterialPreferredPageFetchRet', 'stations/BaseStockForPlayer', 'items/InventoryRollback']
        dep_doc = {'symbols': dep_symbols, 'features': {'reads': {'symbols': dep_names}}}
        resolved, _, supported = Resolver(image, dep_doc).run()
        check(supported['reads'], side + ' complete native read/rollback dependencies resolve')
        for name, rva in zip(dep_names, expected):
            check(resolved[name]['rva'] == rva, f'{side} {name} = {rva:#x}')
        remove, fetch, loop, preferred, stock, rollback = expected
        check(image.img[loop:loop + 7] == bytes.fromhex('44386C24607554'),
              side + ' a failed page fetch is silently skipped by InventoryRemoveItems')
        check(image.img[remove + 0x25a:remove + 0x271] == bytes.fromhex('4533C045897C240833D245896C2404488D4DB045882C24'),
              side + ' the skip path can return error zero and an unpaid remainder')
        check(image.img[fetch + 0xd4:fetch + 0xd8] == bytes.fromhex('41C6061F'),
              side + ' missing native inventory view reports error 0x1f')
        for entry, extent, label in [(remove, 0x2dd, 'material implementation'), (fetch, 0x10c, 'page fetch'),
                                     (stock, 0x174, 'base stock'), (rollback, 0x508, 'rollback')]:
            check(unwind_covers(path, entry, extent), side + ' full code-state extent covers ' + label)
        for ret in [loop, preferred]:
            altered = copy.copy(image)
            b = bytearray(image.img)
            b[ret - 5] = 0x90
            altered.img = bytes(b)
            check(not Resolver(altered, dep_doc).run()[2]['reads'], f'{side} changed fetch call at {ret - 5:#x} disables reads')
        altered = copy.copy(image)
        b = bytearray(image.img)
        b[loop - 4] ^= 1
        altered.img = bytes(b)
        check(not Resolver(altered, dep_doc).run()[2]['reads'], side + ' disagreeing page-fetch targets disable reads')
    print(f'craft preview sigdb: {checks} passed, 0 failed')


if __name__ == '__main__':
    main()
