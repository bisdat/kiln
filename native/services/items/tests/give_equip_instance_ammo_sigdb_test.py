#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""#139: qualify items.give_equip.instance_ammo (instance-backed arrows, spells and bait, eternal spells) on the
static copies of the approved builds: the game's EquipItem/UnequipItem equip such stacks in place exactly like plain
ammo, with the stack's own pide in op7, and the commit's op7 sets or clears ItemState.IsEquipped. Files are only read.

    python3 -I give_equip_instance_ammo_sigdb_test.py --client <enshrouded.exe> --server <enshrouded_server.exe>
"""
import argparse
import copy
import hashlib
import re
import struct
import sys
from pathlib import Path
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/sigport'))
from peimg import Image  # noqa: E402
from sides import compose  # noqa: E402
from sigdb_check import Resolver  # noqa: E402

checks = 0
FEATURE = 'items.give_equip.instance_ammo'
NAMES = ['items/EquipLocationKind', 'items/EquipInPlaceOwned', 'items/EquipItem', 'items/UnequipItem',
         'items/SelectEquipment', 'items/InventoryCommit']
BUILDS = {
    'client': ('5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77',
               [0x416bb0, 0x416f60, 0x4152d0, 0x419740, 0x412d50, 0x40fe30]),
    'server': ('f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2',
               [0x1b6760, 0x1b6ef0, 0x1b4e70, 0x1b9b80, 0x1b28f0, 0x1af860]),
}
# EquipmentSlotType -> the inventory link category the game equips it in: 4 Generic (in place), 2 Equipment (moved)
IN_PLACE = set(range(1, 13)) | {36}
MOVED = set(range(13, 19)) | {34, 35, 37, 38}


def check(ok, text):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(text)
    print('PASS ' + text, flush=True)


def header_bytes(name):
    text = (ROOT / 'native/services/items/qualified_code.h').read_text()
    body = re.search(name + r'\[\] = \{([^}]*)\}', text).group(1)
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body))


def definitions(side):
    """The feature's own file plus the items.json symbols it depends on, as composed for this side."""
    parts = {name: doc for name, doc, _ in compose(side)}
    own = parts['give-equip-instance-ammo.json']
    items = parts['items.json']['symbols']
    symbols = copy.deepcopy(own['symbols'])
    for name in ['items/EquipItem', 'items/UnequipItem', 'items/SelectEquipment', 'items/InventoryCommit']:
        symbols[name] = copy.deepcopy(items[name])
    return {'symbols': symbols, 'features': {FEATURE: {'symbols': own['features'][FEATURE]['symbols']}}}


def r15_writes(img, start, end):
    """Instructions in [start, end) that write r15 (any width), by linear decode; None without capstone."""
    try:
        import capstone
    except ImportError:
        return None
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    hits, at = [], start
    for insn in md.disasm(img[start:end], start):
        at = insn.address + insn.size
        _, written = insn.regs_access()
        if any(md.reg_name(r) in ('r15', 'r15d', 'r15w', 'r15b') for r in written):
            hits.append(f'{insn.address:#x} {insn.mnemonic} {insn.op_str}')
    return hits if at == end else [f'decode stopped at {at:#x}']


def altered(image, at, value):
    out = copy.copy(image)
    b = bytearray(image.img)
    b[at] = value
    out.img = bytes(b)
    return out


def kind_table(img, kind):
    """Decode EquipLocationKind: [rdx+rax+table] byte index -> [rdx+rax*4+targets] -> the 'mov al, N; ret' it reaches."""
    assert img[kind:kind + 12] == bytes.fromhex('0FB6810401000083F8267721')
    index = struct.unpack_from('<I', img, kind + 0x17)[0]    # movzx eax, byte [rdx+rax+index]
    targets = struct.unpack_from('<I', img, kind + 0x1e)[0]  # mov ecx, [rdx+rax*4+targets]
    out = {}
    for t in range(0x27):
        target = struct.unpack_from('<I', img, targets + 4 * img[index + t])[0]
        op = img[target:target + 3]
        out[t] = 0 if op == bytes.fromhex('32C0C3') else op[1] if op[0] == 0xB0 and op[2] == 0xC3 else None
    return out


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--client', required=True)
    args.add_argument('--server', required=True)
    args = args.parse_args()
    kind_code = header_bytes('kEquipLocationKindCode')
    select_code = header_bytes('kSelectEquipmentCode')
    check(len(kind_code) == 12 and len(select_code) == 33, 'qualified entry byte arrays parse from qualified_code.h')
    for side, path in [('client', Path(args.client)), ('server', Path(args.server))]:
        sha, expected = BUILDS[side]
        check(hashlib.sha256(path.read_bytes()).hexdigest() == sha, f'{side}: qualified exe identity')
        image = Image(path)
        img = image.img
        doc = definitions(side)
        found, _, features = Resolver(image, doc).run()
        check(features[FEATURE], f'{side}: {FEATURE} resolves')
        for name, rva in zip(NAMES, expected):
            check(found[name]['rva'] == rva, f'{side}: {name} = {rva:#x}')
        kind, owned, equip, unequip, select, commit = expected
        rel = lambda at: at + 5 + struct.unpack_from('<i', img, at + 1)[0]
        # the game's own location decision, as kiln.items calls it
        check(img[kind:kind + 12] == kind_code, f'{side}: EquipLocationKind entry = kEquipLocationKindCode')
        check(img[select:select + 33] == select_code, f'{side}: SelectEquipment entry = kSelectEquipmentCode')
        table = kind_table(img, kind)
        check(all(table[t] == 4 for t in (6, 7, 8)), f'{side}: Arrow (6), Spell (7), FishingBait (8) are equipped in place (4)')
        check(all(table[t] == (4 if t in IN_PLACE else 2 if t in MOVED else 0) for t in table),
              f'{side}: the whole kind table: 1-12, 36 in place; 13-18, 34, 35, 37, 38 moved; others not equippable')
        check(img[kind + 0x0a:kind + 0x0c] == bytes.fromhex('7721') and img[kind + 0x2d:kind + 0x30] == bytes.fromhex('32C0C3'),
              f'{side}: a slot type above 0x26 is not equippable')
        # EquipItem: the in-place branch passes r15d = the stack's own pide field, with no HasPIDE test on the way
        site = equip + 0x14a
        check(rel(site + 3) == kind and img[site + 0x13:site + 0x15] == bytes.fromhex('3C04'),
              f'{side}: EquipItem asks EquipLocationKind and branches on 4')
        check(rel(site + 0x21) == owned and rel(site + 0x57) == select, f'{side}: in place: owned cell check, then SelectEquipment')
        check(img[site + 0x31:site + 0x34] == bytes.fromhex('4533C0') and img[site + 0x3c:site + 0x41] == bytes.fromhex('44897C2440'),
              f'{side}: in place: remove 0 and the 9th argument (pide) from r15d')
        check(img[equip + 0xa5:equip + 0xaf] == bytes.fromhex('488B56088B0B448B7B08'),
              f'{side}: r15d = getItemStack()->pide (+8), before ItemRegistryFind')
        r15 = r15_writes(img, equip + 0xaf, site + 0x3c)
        if r15 is None:
            print(f'SKIP {side}: r15 not rewritten before the pide argument (needs the capstone module)')
        else:
            check(not r15, f'{side}: nothing between the pide load and the in-place select writes r15 ({r15})')
        check(b'\xd8\x05\x00\x00\x02' not in img[equip:site + 0x5c],
              f'{side}: EquipItem tests no HasPIDE bit (ItemInfo+0x5d8 & 2) before the in-place select')
        check(b'\xd8\x05\x00\x00\x02' not in img[select:select + 0x171] and
              img[select + 0x2e:select + 0x37] == bytes.fromhex('41F68424D805000004'),
              f'{side}: SelectEquipment tests only the own-inventory flag 4, never HasPIDE')
        # UnequipItem: kind 4 deselects in place with the stack's pide (remove 1, the stack stays where it is)
        usite = unequip + 0x14f
        check(rel(usite + 3) == kind and img[usite + 0x0b:usite + 0x0d] == bytes.fromhex('3C04') and
              img[usite + 0x16:usite + 0x19] == bytes.fromhex('41B001') and rel(usite + 0x3c) == select,
              f'{side}: UnequipItem kind 4: SelectEquipment(remove 1, pide), no move')
        # InventoryCommit op7: ItemState (tx[5]) +0x0e |= 8 for a cell, &= 0xfff7 for remove
        cop7 = commit + 0xad7
        check(img[cop7 + 0x0d:cop7 + 0x0e] == b'\xe8' and img[cop7 + 0x12:cop7 + 0x18] == bytes.fromhex('41837C242000') and
              img[cop7 + 0x1e:cop7 + 0x27] == bytes.fromhex('458B442420498B5528'),
              f'{side}: commit op7: a nonzero op.pide reads ItemState through tx+0x28')
        check(img[cop7 + 0x48:cop7 + 0x4c] == bytes.fromhex('6621780E') and img[cop7 + 0x51:cop7 + 0x56] == bytes.fromhex('6683480E08'),
              f'{side}: commit op7: clear (and di) for remove, or 0x08 for a cell, at ItemState+0x0e')
        check(img.find(bytes.fromhex('BFF7FF0000'), commit, commit + 0x100) > 0,
              f'{side}: commit loads di = 0xfff7 (the IsEquipped clear mask)')
        # mutations: each site's load-bearing byte switches the feature off
        for at, value, what in [(site + 0x14, 0x05, 'EquipItem kind compare'),
                                (site + 0x3c, 0x90, 'EquipItem pide argument'),
                                (equip + 0xab, 0x00, 'EquipItem pide load'),
                                (usite + 0x18, 0x00, 'UnequipItem remove flag'),
                                (cop7 + 0x55, 0x10, 'commit IsEquipped bit'),
                                (kind + 0x28, 0x05, 'EquipLocationKind in-place return')]:
            check(not Resolver(altered(image, at, value), doc).run()[2][FEATURE], f'{side}: changed {what} at {at:#x} disables {FEATURE}')
    print(f'give-equip instance ammo sigdb: {checks} passed, 0 failed')


if __name__ == '__main__':
    main()
