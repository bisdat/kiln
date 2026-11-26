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
        doc = {'symbols': symbols, 'features': {'build': {'symbols': NAMES}, 'tx': {'symbols': ['items/InventoryTxInit']}}}
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
        # GitHub #148: the blueprint program's transaction has no Fuel accessor. Its setup (rbp+0x290, passed to
        # items/InventoryTxInit right before the payment) has +0x60..+0xbf stored from zeroed xmm0/xmm1 (+0x70, the Fuel
        # accessor, by movdqa [rbp+0x300],xmm1), and InventoryTxInit copies setup+0x70 to tx+0x38, the word kiln.items'
        # access_from_tx reads as Fuel (w[7]); so only access_from_tx_reads (InventorySetup and Inventory) accepts it
        tx_init = found['items/InventoryTxInit']['rva']
        init_call = bp_ret - 0x61
        check(raw[init_call] == 0xE8 and rel(init_call) == tx_init, side + ' the blueprint program opens its transaction with InventoryTxInit')
        check(raw[init_call - 0x60:init_call].find(bytes.fromhex('488D9590020000')) >= 0, side + ' ... from its setup at rbp+0x290')
        setup = raw[bp_ret - 0x17e:init_call]
        check(setup.startswith(bytes.fromhex('0F9485910200')), side + ' the setup\'s stock mode is the sete at rbp+0x291')
        zero = setup.find(bytes.fromhex('660F7F8D00030000'))
        before = raw[bp_ret - 0x1a0:bp_ret - 0x17e]  # xorps xmm0,xmm0 / xorps xmm1,xmm1 come right before the sete
        check(zero > 0 and bytes.fromhex('0F57C9') in before and b'\x0f\x28' not in setup[:zero] and b'\x0f\x10' not in setup[:zero],
              side + ' setup+0x70 (Fuel) is stored from a zeroed xmm1 (no xmm load in between)')
        check(setup.count(bytes.fromhex('00030000')) == 1, side + ' nothing else writes setup+0x70 before InventoryTxInit')
        check(raw[tx_init:tx_init + 0x160].find(bytes.fromhex('488B427048894138')) > 0, side + ' InventoryTxInit copies setup+0x70 to tx+0x38')
        engine = (ROOT / 'native/services/items/engine.cpp').read_text()
        check('a->acc[A_FUEL] = reinterpret_cast<uint64_t>(w[7]);' in engine, 'access_from_tx reads Fuel at tx+0x38 (w[7])')
        for at in [cursor_ret - 5, bp_ret - 5]:
            altered = copy.copy(image)
            b = bytearray(raw)
            b[at] = 0x90
            altered.img = bytes(b)
            check(not Resolver(altered, doc).run()[2]['build'], f'{side} changed call at {at:#x} disables crafting.build_sources')
    # the build menu's material counts (crafting.build_ui, client only)
    client = Path(args.client)
    symbols = {}
    for _, part, _ in compose('client'):
        symbols.update(part.get('symbols', {}))
    ui_names = ['build/client.MaterialRows', 'build/client.MaterialRowsBody', 'build/client.MaterialRowsDone', 'build/client.StockRowsRet',
                'build/client.MaterialRowAdd', 'build/client.MaterialRowStock']
    doc = {'symbols': symbols, 'features': {'ui': {'symbols': ui_names}}}
    image = Image(client)
    found, _, features = Resolver(image, doc).run()
    check(features['ui'], 'client crafting.build_ui resolves')
    for name, rva in zip(ui_names, [0x40e5d0, 0x40e5f3, 0x40f05a, 0x40ee4b, 0x40c010, 0x40c0f4]):
        check(found[name]['rva'] == rva, f'client {name} = {rva:#x}')
    raw = image.img
    # the gatherer's two halves are inside it, clear of what Building Tools 0.34.0 reads (src/main.cpp SIG_STOCKS: the first
    # 35 bytes, then + 0x194, + 0x705, + 0x721, + 0x78f, + 0x867, + 0x876, + 0x89d and the call it redirects at + 0xa76);
    # presentation_sigdb_test.py checks the two sites' registers and branches
    fn = found['build/client.MaterialRows']['rva']
    read = [(0, 35), (0x194, 3), (0x705, 5), (0x721, 5), (0x78F, 7), (0x867, 7), (0x876, 5), (0x89D, 5), (0xA76, 5)]
    for name in ('build/client.MaterialRowsBody', 'build/client.MaterialRowsDone'):
        at = found[name]['rva'] - fn
        check(all(at + 5 <= o or at >= o + n for o, n in read), f'client {name} (+ {at:#x}) is off the bytes Building Tools reads')
    check(bytes.fromhex('4869C848060000') in raw[0x40ee11:0x40ee4b], 'a group is 0x648 bytes (imul rcx,rax,0x648)')
    check(raw[0x40e694:0x40e69b] == bytes.fromhex('4989AFD0F9FFFF'), 'a rebuilt group is zeroed first (mov [r15-0x630],rbp: its count)')
    check(raw[0x40c0f9:0x40c105] == bytes.fromhex('488D0C40488B0748016CC810'), 'a row is 24 bytes, its stock a u64 at +0x10')
    check(raw[0x40e615:0x40e61c] == bytes.fromhex('4438B6F82B0000'), 'seven groups (the current group byte after them at +0x2bf8 = 7 x 0x648)')
    server_names = set()
    for _, part, _ in compose('server'):
        server_names.update(part.get('symbols', {}))
    check(not any(n in server_names for n in ui_names), 'the build menu sites are declared for clients only')
    altered = copy.copy(image)
    b = bytearray(raw)
    b[0x40ee46] = 0x90
    altered.img = bytes(b)
    check(not Resolver(altered, doc).run()[2]['ui'], 'a changed row-add call disables crafting.build_ui')
    print(f'build sources sigdb: {checks} passed, 0 failed')


if __name__ == '__main__':
    main()
