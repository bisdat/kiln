#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""GitHub #132 (0.2.0-dev.31.17): qualify stations.fill_items (sigdb server.d/stations.json, shared by the client) on static
copies of the pinned client (26430625, 5d9ef2f1) and dedicated server (26430253, f0d824be): the files are read, never run.

What it pins (docs/re/stations.md section 10.9): the station screen's per-ingredient transfer reaches the server as
player_crafting's AutomaticTransfer with an item; AutoTransferAction's item branch (cmp [rsi+8],0) calls AutoTransferItem
(stations/AutoTransferItem) as (rcx = the transaction rbp+0x3b0, rdx = &{list A ptr, count} at [rsp+0x40], r8d = the item
[rsi+8], r9d = the station [rsi], [rsp+0x20] = the u16 amount [rsi+0x0c], [rsp+0x28] = the player [rbp+0x7590]) and rolls
the transaction back on any code but 0; the function pays the player first (PayMaterial when the player is set), then each
list-A entity in order, answers 4 when something is still missing and 2 when the station cannot take it all. A changed byte
at the call turns the feature off."""
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
EXPECTED = {
    'server': {'sha': 'f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2', 'item': 0x198bf0, 'action': 0xb2080,
               'call': 0xb2408, 'pay': 0x1b0d60, 'add': 0x1b1f60, 'rollback': 0x1b8310},
    'client': {'sha': '5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77', 'item': 0x3f7950, 'action': 0x2c6b30,
               'call': 0x2c6eb8, 'pay': 0x411330, 'add': 0x412460, 'rollback': 0x418150},
}


def check(ok, text):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(text)
    print('PASS ' + text, flush=True)


def doc_for(side):
    parts = {name: doc for name, doc, _ in compose(side)}
    st = parts['stations.json']
    return {'symbols': copy.deepcopy(st['symbols']), 'constants': copy.deepcopy(st['constants']),
            'features': {'items': copy.deepcopy(st['features']['stations.fill_items']),
                         'fill': copy.deepcopy(st['features']['stations.fill'])}}


def rel(raw, at):
    return at + 5 + struct.unpack_from('<i', raw, at + 1)[0]


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--client', required=True)
    args.add_argument('--server', required=True)
    args = args.parse_args()
    for side, path in [('server', Path(args.server)), ('client', Path(args.client))]:
        want = EXPECTED[side]
        check(hashlib.sha256(path.read_bytes()).hexdigest() == want['sha'], f'{side}: qualified identity')
        image = Image(path)
        doc = doc_for(side)
        found, consts, features = Resolver(image, doc).run()
        check(features['items'], f'{side}: stations.fill_items resolves')
        check(features['fill'], f'{side}: stations.fill still resolves')
        check(found['stations/AutoTransferItem']['ok'] and found['stations/AutoTransferItem']['rva'] == want['item'],
              f'{side}: stations/AutoTransferItem = {want["item"]:#x}')
        check(found['stations/AutoTransferAction']['rva'] == want['action'], f'{side}: stations/AutoTransferAction = {want["action"]:#x}')
        check(consts['stations/action.amount']['value'] == 0x0c, f'{side}: the action\'s amount at +0x0c (PlayerCraftingAction)')
        raw = image.img
        call = want['call']
        check(raw[call] == 0xE8 and rel(raw, call) == want['item'], f'{side}: the press\'s item branch calls AutoTransferItem at {call:#x}')
        # the branch: cmp [rsi+8],r12d (itemId); je -> water; then the arguments as the call takes them
        head = raw[call - 0x36:call]
        check(head[:6] == bytes.fromhex('443966087437'), f'{side}: taken only for an item (cmp [rsi+8],r12d; je water)')
        check(bytes.fromhex('8B8590750000') in head and bytes.fromhex('488D542440') in head and bytes.fromhex('448B0E') in head and
              bytes.fromhex('488D8DB0030000') in head and bytes.fromhex('448B4608') in head and bytes.fromhex('89442428') in head and
              bytes.fromhex('0FB7460C6689442420') in head and bytes.fromhex('48897C24404C897C2448') in head,
              f'{side}: (tx, &{{list A, count}}, item, station, u16 amount, player)')
        # after it: jmp to the common test: mov ebx,eax; test ebx,ebx; je commit; lea rcx,[rbp+0x3b0]; call rollback
        check(raw[call + 5] == 0xEB, f'{side}: the branch joins the common result test')
        j = call + 7 + struct.unpack_from('<b', raw, call + 6)[0]
        check(raw[j:j + 13] == bytes.fromhex('8BD885DB740E488D8DB0030000') and rel(raw, j + 13) == want['rollback'],
              f'{side}: a code other than 0 rolls the transaction back ({want["rollback"]:#x})')
        # the function: PayMaterial from the player when set, then the list loop, 4 when short, the station's add, 2 on a leftover
        body = raw[want['item']:want['item'] + 0x150]
        targets = [rel(raw, want['item'] + i) for i in range(len(body) - 5) if body[i] == 0xE8]
        check(targets.count(want['pay']) == 2, f'{side}: PayMaterial twice: the player, then each list entity')
        check(want['add'] in targets, f'{side}: then the station\'s add ({want["add"]:#x})')
        check(bytes.fromhex('B804000000') in body and bytes.fromhex('B802000000') in body, f'{side}: answers 4 short, 2 no room')
        check(bytes.fromhex('4D8B3E') in body and bytes.fromhex('493B6E08') in body and bytes.fromhex('4983C704') in body,
              f'{side}: the list is {{u32* ids, count}} walked 4 bytes at a time')
        # a changed byte at the call disables the feature, not the recipe fill
        altered = copy.copy(image)
        b = bytearray(raw)
        b[call - 0x16] ^= 0xFF  # the player's store (mov [rsp+0x28],eax)
        altered.img = bytes(b)
        _, _, f2 = Resolver(altered, doc).run()
        check(not f2['items'] and f2['fill'], f'{side}: a changed call disables stations.fill_items only')
    print(f'item fill sigdb: {checks} passed, 0 failed')


if __name__ == '__main__':
    main()
