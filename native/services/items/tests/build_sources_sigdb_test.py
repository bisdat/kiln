#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""GitHub #132: crafting.build_sources on static copies of the two pinned executables (read as files, never run)."""
import argparse
import copy
import hashlib
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


NAMES = ['build/CursorStockRet', 'build/StockCount', 'build/BlueprintApplyRet', 'rules/ApplyMaterials', 'build/ApplyMaterialsPaid',
         'rules/InventoryPay']
EXPECTED = {
    'server': {'sha': 'f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2',
               'rva': [0x99a50, 0x209210, 0xc4b50, 0x22d480, 0x22d562, 0x199c80], 'cursor': 0x99750},
    'client': {'sha': '5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77',
               'rva': [0x28ae3c, 0x4709c0, 0x2d8bb0, 0x495f20, 0x496002, 0x3f89e0], 'cursor': 0x28ab60},
}


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--client', required=True)
    args.add_argument('--server', required=True)
    args = args.parse_args()
    for side, path in [('server', Path(args.server)), ('client', Path(args.client))]:
        want = EXPECTED[side]
        check(hashlib.sha256(path.read_bytes()).hexdigest() == want['sha'], side + ' qualified identity')
        symbols = {}
        for _, part, _ in compose(side):
            symbols.update(part.get('symbols', {}))
        doc = {'symbols': symbols, 'features': {'build': {'symbols': NAMES}}}
        image = Image(path)
        found, _, features = Resolver(image, doc).run()
        check(features['build'], side + ' crafting.build_sources resolves')
        for name, rva in zip(NAMES, want['rva']):
            check(found[name]['rva'] == rva, f'{side} {name} = {rva:#x}')
        raw = image.img
        rel = lambda at: at + 5 + struct.unpack_from('<i', raw, at + 1)[0]
        cursor_ret, stock, bp_ret, apply, paid, pay = want['rva']
        check(rel(cursor_ret - 5) == stock and rel(bp_ret - 5) == apply and rel(paid - 5) == pay, side + ' the three calls')
        # the registers the cursor hook reads: base count in eax, bag count in ebx, blocks needed in ebp
        check(raw[cursor_ret:cursor_ret + 6] == bytes.fromhex('03C33BC50F83'), side + ' after the stock count: add eax,ebx; cmp eax,ebp; jae')
        body = raw[want['cursor']:cursor_ret]
        check(bytes.fromhex('4C8B4668') in body[-0x70:] and bytes.fromhex('8B5C2458') in body[-0x60:],
              side + ' the material is [[rsi+0x68]] and the bag count [rsp+0x58] -> ebx')
        check(bytes.fromhex('488BF9') in raw[want['cursor']:want['cursor'] + 0x20], side + ' rdi is the cursor context (mov rdi,rcx)')
        check(raw[want['cursor'] + (0x10 if side == 'server' else 0xf):][:3] in (bytes.fromhex('4C8B31'), bytes.fromhex('488B19')),
              side + ' the cursor context\'s first qword is the program context')
        # ApplyMaterials' take in mode 0: InventoryPay(tx, list, [tx+0xe0], 1, 0); the blueprint call passes take = r12b
        check(raw[paid - 17:paid - 5] == bytes.fromhex('448B81E0000000') + bytes.fromhex('48895C2420'), side + ' the take names the operator')
        check(raw[bp_ret - 17:bp_ret - 13] == bytes.fromhex('450FB6C4'), side + ' the blueprint program passes its take flag')
        for at in [cursor_ret - 5, bp_ret - 5]:
            altered = copy.copy(image)
            b = bytearray(raw)
            b[at] = 0x90
            altered.img = bytes(b)
            check(not Resolver(altered, doc).run()[2]['build'], f'{side} changed call at {at:#x} disables crafting.build_sources')
    print(f'build sources sigdb: {checks} passed, 0 failed')


if __name__ == '__main__':
    main()
