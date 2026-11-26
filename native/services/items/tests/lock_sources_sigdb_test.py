#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""GitHub #147: qualify items.lock_sources (sigdb server.d/ and client.d/lock-sources.json) on static copies of the
pinned client (26430625, 5d9ef2f1) and dedicated server (26430253, f0d824be): the files are read, never run.

What it pins: the lock check's item count call (locks/LockCountRet) and what the mid hook there reads (rbp-0x68 the
result, r14 the InteractionLock, [rsp+0x58] the context), interaction_attempt's lock payment (locks/LockPaid: rbp+0x230
the transaction, [rsp+0x58] PayMaterial's result, r14 the guest, rsi the lock, the Consume test after it), the locked
prompt's count (locks/client.UiLockCountRet, client only), the prompt's requirement row and its own bag count that
decides the row's colour (locks/client.LockRowCount, client only), the InteractionLock offsets the patterns capture, and that a
changed byte at each call turns the feature off (a changed requirement row only that symbol: the line is display only)."""
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
CLIENT = '5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77'
SERVER = 'f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2'


def check(ok, text):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(text)
    print('PASS ' + text, flush=True)


def doc_for(side):
    parts = {name: doc for name, doc, _ in compose(side)}
    lock = parts['lock-sources.json']
    symbols = copy.deepcopy(lock['symbols'])
    # gems/PayMaterial is defined by captures elsewhere (gems/PaySlot); here LockPaid's own rel32 hole captures it,
    # and the test compares that with the build's known address
    return {'symbols': symbols, 'constants': copy.deepcopy(lock['constants']),
            'features': {'locks': {'symbols': [n for n in lock['features']['items.lock_sources']['symbols']
                                               if n != 'world/CurrentEntityId'],
                                   'constants': ['locks/lock.item', 'locks/lock.amount', 'locks/lock.mask']}}}


def rel(raw, at):
    return at + 5 + struct.unpack_from('<i', raw, at + 1)[0]


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--client', required=True)
    args.add_argument('--server', required=True)
    args = args.parse_args()
    client, server = Path(args.client), Path(args.server)
    check(hashlib.sha256(client.read_bytes()).hexdigest() == CLIENT, 'qualified client identity (26430625)')
    check(hashlib.sha256(server.read_bytes()).hexdigest() == SERVER, 'qualified dedicated identity (26430253)')
    c, s = Image(client), Image(server)
    found_client = {}
    expect = {
        'client': (c, {'locks/LockCountRet': 0x2c5547, 'locks/InventoryItemCount': 0xe93160, 'locks/LockPaid': 0x2a7efe,
                       'locks/client.UiLockCountRet': 0x2bc483, 'gems/PayMaterial': 0x411330}),
        'server': (s, {'locks/LockCountRet': 0xb0a97, 'locks/InventoryItemCount': 0x93a2a0, 'locks/LockPaid': 0xa536e,
                       'gems/PayMaterial': 0x1b0d60}),
    }
    for side, (image, rvas) in expect.items():
        doc = doc_for(side)
        found, consts, features = Resolver(image, doc).run()
        if side == 'client':
            found_client = found
        check(features['locks'], f'{side}: items.lock_sources resolves')
        for name, rva in rvas.items():
            check(found[name]['ok'] and found[name]['rva'] == rva, f'{side}: {name} = {rva:#x}')
        check(consts['locks/lock.item']['value'] == 0x10 and consts['locks/lock.amount']['value'] == 0x16 and
              consts['locks/lock.mask']['value'] == 0x18,
              f'{side}: InteractionLock requiredItem +0x10, requiredItemAmount +0x16, lockMask +0x18 (the issue\'s +16/+22/+24)')
        raw = image.img
        ret, count, paid, pay = rvas['locks/LockCountRet'], rvas['locks/InventoryItemCount'], rvas['locks/LockPaid'], rvas['gems/PayMaterial']
        check(rel(raw, ret - 5) == count and rel(raw, paid - 5) == pay, f'{side}: both returns follow calls to their targets')
        # the lock check: lea rcx,[rbp-0x68] (the result); mov rdx,[rsp+0x58] (the context); then cmp [rbp-0x68],bl;
        # cmovz edi,[rbp-0x60] (the count); movzx ebx,word [r14+0x16] (the amount) ... cmp edi,ebx; setb r15b (locked)
        check(raw[ret - 0x32:ret - 0x2e] == bytes.fromhex('488D4D98') and raw[ret - 0x23:ret - 0x1e] == bytes.fromhex('488B542458'),
              f'{side}: the count writes rbp-0x68, the context is [rsp+0x58]')
        check(raw[ret:ret + 3] == bytes.fromhex('385D98') and raw[ret + 9:ret + 13] == bytes.fromhex('0F447DA0') and
              raw[ret + 15:ret + 20] == bytes.fromhex('410FB75E16'), f'{side}: error at rbp-0x68, count at rbp-0x60, amount from r14')
        tail = raw[ret:ret + 0x60]
        cmp = tail.find(bytes.fromhex('3BFB'))
        check(cmp > 0 and bytes.fromhex('410F92C7') in tail[cmp:cmp + 16],
              f'{side}: count < amount sets the locked byte (cmp edi,ebx; ...; setb r15b)')
        # InventoryItemCount: {u8 error; u32 0; u32 count}: the not-initialized path writes 0x15, the end count at +8
        check(bytes.fromhex('C60115') in raw[count:count + 0x40],
              f'{side}: the count answers error 0x15 for an uninitialized setup')
        # the payment: lea rdx,[rbp+0x230] (the transaction) before the call; error at [rsp+0x58], left at [rsp+0x60];
        # then test byte [rsi+0x18],1: Consume commits, else the presence test rolls back
        check(raw[paid - 0x1c:paid - 0x15] == bytes.fromhex('488D9530020000') and raw[paid - 0x15:paid - 0x11] == bytes.fromhex('448B4E10'),
              f'{side}: PayMaterial(tx = rbp+0x230, guest, requiredItem, requiredItemAmount)')
        check(raw[paid:paid + 5] == bytes.fromhex('807C245800') and raw[paid + 11:paid + 16] == bytes.fromhex('837C246000') and
              raw[paid + 22:paid + 26] == bytes.fromhex('F6461801'), f'{side}: error and left tested, then lockMask bit 0 (Consume)')
        check(raw[paid + 26:paid + 33] == bytes.fromhex('488D8D30020000') and raw[paid + 33] == 0x74 and
              rel(raw, paid + 35) == rvas.get('items/InventoryCommit', rel(raw, paid + 35)),
              f'{side}: no Consume branches past the commit (to the rollback)')
        # a changed byte at each call disables the feature
        for at in [ret - 5, paid - 5] + ([rvas['locks/client.UiLockCountRet'] - 5] if side == 'client' else []):
            altered = copy.copy(image)
            b = bytearray(raw)
            b[at] = 0x90
            altered.img = bytes(b)
            check(not Resolver(altered, doc).run()[2]['locks'], f'{side}: a changed call at {at:#x} disables items.lock_sources')
    # the prompt is client-only UI code
    ui = {'symbols': {'locks/client.UiLockCountRet': {name: doc for name, doc, _ in compose('client')}['lock-sources.json']
                      ['symbols']['locks/client.UiLockCountRet']}, 'features': {'ui': {'symbols': ['locks/client.UiLockCountRet']}}}
    ui['symbols'].update({k: v for k, v in doc_for('client')['symbols'].items() if k == 'locks/LockCountRet'})
    check(not Resolver(s, ui).run()[2]['ui'], 'the locked prompt\'s site is absent from the dedicated server')
    raw = c.img
    at = 0x2bc483
    check(raw[at - 0x49:at - 0x46] == bytes.fromhex('8B5F10') and bytes.fromhex('488D8D20060000') in raw[at - 0x40:at - 0x20] and
          raw[at + 6:at + 10] == bytes.fromhex('0FB74716') and bytes.fromhex('39852806000073') in raw[at:at + 0x30],
          'client: the prompt counts into rbp+0x620 (count +0x628) for the lock in rdi and compares with its amount')
    # the prompt's requirement row (the HUD, client only): its own count of the item in the local player's bag (the UI's
    # inventory mirror, C 0xd40fe0) decides the row's colour; locks/client.LockRowCount is right after that count, where
    # r8d holds it and r14 the prompt record (player_ui's +0x2bc0: +0xd8 item = +0x2c98, +0xdc amount = +0x2c9c)
    row = 0xbf73be
    check(found_client['locks/client.LockRowCount']['ok'] and found_client['locks/client.LockRowCount']['rva'] == row,
          f'client: locks/client.LockRowCount = {row:#x}')
    check(rel(raw, row - 0x44) == 0xd40fe0 and raw[row - 0x3f:row - 0x38] == bytes.fromhex('458B9ED8000000') and
          raw[row - 0x38:row - 0x35] == bytes.fromhex('448BC7'),
          'client: the row counts after the bag mirror (C 0xd40fe0): item from r14+0xd8, the count in r8d from 0')
    check(bytes.fromhex('450344D210') in raw[row - 0x20:row] and raw[row:row + 8] == bytes.fromhex('410FB78EDC000000'),
          'client: the loop adds each matching stack to r8d; the site reads the amount from r14+0xdc')
    tail = raw[row:row + 0x30]
    check(bytes.fromhex('44898544020000') in tail and bytes.fromhex('443BC1') in tail and bytes.fromhex('0F938548020000') in tail,
          'client: the row stores r8d as its count and sets met = count >= max(amount, 1) (cmp r8d,ecx; setae)')
    check(rel(raw, 0xbf7469) == 0xbf1ea0 and raw[0xbf2141:0xbf2146] == bytes.fromhex('450FB67E30') and
          raw[0xbf2455:0xbf2459] == bytes.fromhex('41387E30'),
          'client: the row draw (C 0xbf1ea0) picks its colour and text style by that met byte (row +0x30)')
    check(raw[0x2bc483:0x2bc489] == bytes.fromhex('899E982C0000') and raw[0x2bc48d:0x2bc494] == bytes.fromhex('6689869C2C0000') and
          raw[0xbf7134:0xbf713b] == bytes.fromhex('4D8DB7C02B0000'),
          'client: player_ui writes the item +0x2c98 and amount +0x2c9c; the HUD reads them as r14 = +0x2bc0 (+0xd8, +0xdc)')
    altered = copy.copy(c)
    b = bytearray(raw)
    b[row + 1] = 0x90
    altered.img = bytes(b)
    # review of 31.15: the line is display only and a hosting client's server half shares items.lock_sources, so the
    # row's symbol is not one of the feature's: a build where only it breaks keeps the feature, and the line alone keeps
    # the game's count (kiln-items' lock_resolve)
    found_alt, _, features_alt = Resolver(altered, doc_for('client')).run()
    check(features_alt['locks'] and not found_alt['locks/client.LockRowCount']['ok'] and 'locks/client.LockRowCount' not in
          {name: doc for name, doc, _ in compose('client')}['lock-sources.json']['features']['items.lock_sources']['symbols'],
          'client: a changed requirement row leaves items.lock_sources on; only locks/client.LockRowCount does not resolve')
    check(not Resolver(s, {'symbols': {'locks/client.LockRowCount': copy.deepcopy(
              {name: doc for name, doc, _ in compose('client')}['lock-sources.json']['symbols']['locks/client.LockRowCount'])},
              'features': {'row': {'symbols': ['locks/client.LockRowCount']}}}).run()[2]['row'],
          'the requirement row\'s site is absent from the dedicated server')
    print(f'lock sources sigdb: {checks} passed, 0 failed')


if __name__ == '__main__':
    main()
