#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""kiln.skills.client (0.2.0-dev.31.13): qualify skills.client_view on static copies of the pinned executables.

Read as files, never run. On client 26430625 the game's skilltree_ui (skills/TreeUi) writes the local player's points
and one record per node into G38UiPlayData: the point offsets, the record's level and unlocked bytes, the records map's
fields and the node's levelIndex are captured from its own code; its spent sum is the game's PointsSpent (Kiln's shadow
hook point); it runs for the local player only; client_view_logic.h's node offsets are tree.h's. A changed byte turns
the feature off; the dedicated server has no such feature. docs/re/progression.md section 8.
"""
import argparse
import copy
import hashlib
from pathlib import Path
import re
import struct
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/sigport'))
from peimg import Image  # noqa: E402
from sides import compose  # noqa: E402
from sigdb_check import Resolver  # noqa: E402

CLIENT_SHA = '5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77'
SERVER_SHA = 'f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2'
checks = 0


def check(ok, text):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(text)
    print('PASS ' + text, flush=True)


def definitions():
    parts = {name: doc for name, doc, _ in compose('client')}
    own = parts['skills-client-view.json']
    ui = parts['skills.json']  # the client overlay (extends server.d/skills.json): TreeUi and its fields
    doc = {'symbols': copy.deepcopy(own['symbols']), 'constants': copy.deepcopy(own['constants']),
           'features': copy.deepcopy(own['features'])}
    for name in ['skills/TreeUi', '@skills/TreeUi.fields']:
        doc['symbols'][name] = ui['symbols'][name]
    for name in ['skills/ui.records', 'skills/ui.masks', 'skills/ui.record_size']:
        doc['constants'][name] = ui['constants'][name]
    # the tree's fields come from server.d/skills.json (shared by the client), and PointsSpent for the call check
    srv = {name: d for name, d, _ in compose('server')}['skills.json']
    for name, d in srv['symbols'].items():
        doc['symbols'].setdefault(name, d)
    for name, d in srv.get('constants', {}).items():
        doc['constants'].setdefault(name, d)
    return doc


def main():
    a = argparse.ArgumentParser(description=__doc__)
    a.add_argument('--client', required=True)
    a.add_argument('--server', required=True)
    a = a.parse_args()
    client, server = Path(a.client), Path(a.server)
    check(hashlib.sha256(client.read_bytes()).hexdigest() == CLIENT_SHA, 'qualified client identity (Steam 26430625)')
    check(hashlib.sha256(server.read_bytes()).hexdigest() == SERVER_SHA, 'qualified dedicated identity (Steam 26430253)')
    check('skills-client-view.json' not in {n for n, _, _ in compose('server')}, 'the file is client-only (client.d)')
    doc = definitions()
    c, s = Image(client), Image(server)
    found, consts, feats = Resolver(c, doc).run()
    check(feats['skills.client_view'], 'skills.client_view resolves on the pinned client')
    check(found['skills/TreeUi']['ok'] and found['skills/TreeUi']['rva'] == 0x318540, 'skills/TreeUi = 0x318540')
    want = {'skills/ui.records': 0x449198, 'skills/ui.record_size': 0x44, 'skills/ui.unlock_levels': 0x20,
            'skills/ui.points_total': 0x72c, 'skills/ui.points_spent': 0x730, 'skills/ui.points_available': 0x734,
            'skills/ui.map_cap': 0x10, 'skills/ui.map_values': 0x40, 'skills/node.level_index': 0x89,
            'skills/ui.rec_level': 0x41, 'skills/ui.rec_unlocked': 0x43, 'skills/ui.records_cap_at': 0x4491a8,
            'skills/ui.records_bits_at': 0x4491a0, 'skills/ui.records_keys_at': 0x4491c0,
            'skills/ui.records_values_at': 0x4491d8, 'skills/tree.data': 0x20, 'skills/tree.ids': 0x150}
    for name, v in want.items():
        check(consts[name]['ok'] and consts[name]['value'] == v, f'{name} = {v:#x}')
    rec = want['skills/ui.records']
    check((want['skills/ui.records_bits_at'] - rec, want['skills/ui.records_cap_at'] - rec, want['skills/ui.records_keys_at'] - rec,
           want['skills/ui.records_values_at'] - rec) == (0x08, 0x10, 0x28, 0x40),
          "the records map's bits, capacity, keys and values sit where Kiln's screen hook walks them (+8, +0x10, +0x28, +0x40)")
    check(not Resolver(s, doc).run()[2]['skills.client_view'], 'no skills.client_view on the dedicated executable')

    img = c.img
    # the spent sum is the game's PointsSpent (Kiln hooks it, so shadow costs are in), on PlayerSkillsUi's masks / levels
    at = img.find(bytes.fromhex('488D5120E8'), 0x318540, 0x318540 + 0x700)
    target = at + 4 + 5 + struct.unpack_from('<i', img, at + 5)[0]
    check(found['skills/PointsSpent']['ok'] and target == found['skills/PointsSpent']['rva'] == 0xe7b170,
          "skilltree_ui's spent = the game's PointsSpent (0xe7b170), shadow nodes counted by Kiln's hook")
    check(img[at + 9:at + 9 + 28] == bytes.fromhex('8B8F2C0700004533E48987300700004489A7340700003BC876082BC8'),
          'available = total - spent when total > spent, else 0')
    # it runs for the local player only: the entity == LocalPlayerData+4 gate
    gate = 0x3185e6
    check(img[gate] == 0xE8 and gate + 5 + struct.unpack_from('<i', img, gate + 1)[0] == 0x3bb740 and
          img[0x3bb740:0x3bb747] == bytes.fromhex('394A040F94C0C3'), 'skilltree_ui updates the local player only (entity == LocalPlayerData+4)')
    # the record's level is PlayerSkillsUi.unlockLevel[levelIndex] decoded, its unlocked byte cleared before the mask pass
    check(img[0x318650:0x318657] == bytes.fromhex('0FB68B89000000') and img[0x31865b:0x318660] == bytes.fromhex('0FB64C0120') and
          img[0x318665:0x318669] == bytes.fromhex('41884541') and img[0x318682:0x318687] == bytes.fromhex('41C6454300'),
          'record +0x41 = the level slot of node.levelIndex (a plain node: slot 0, not its own), +0x43 = 0 before the masks')
    # client_view_logic.h's node offsets are tree.h's
    logic = (ROOT / 'native/services/skills/client_view_logic.h').read_text()
    tree = (ROOT / 'native/services/skills/tree.h').read_text()
    for field in ['pos', 'in', 'out', 'cost', 'type', 'cls', 'max_level', 'level_index']:
        a1 = re.search(r'\b' + field + r' = (0x[0-9a-f]+)', logic)
        a2 = re.search(r'\b' + field + r' = (0x[0-9a-f]+)', tree)
        check(a1 and a2 and int(a1.group(1), 16) == int(a2.group(1), 16), f'NodeOffsets::{field} is tree.h F::{field}')
    check(int(re.search(r'level_index = (0x[0-9a-f]+)', logic).group(1), 16) == want['skills/node.level_index'],
          "the node's levelIndex offset the game's code reads is the one Kiln decodes")
    # a changed byte turns the feature off
    for at, what in [(0x318b8b, "another instruction after the total's read in skilltree_ui"), (0x318b33, "a moved unlocked byte in the record"),
                     (0x318667, "another register for the record's level write"), (0x3188ad, "a moved keys field of the records map")]:
        altered = copy.copy(c)
        b = bytearray(img)
        b[at] ^= 0x01
        altered.img = bytes(b)
        check(not Resolver(altered, doc).run()[2]['skills.client_view'], f'{what} ({at:#x}) turns skills.client_view off')
    print(f'skills client view sigdb: {checks} passed, 0 failed')


if __name__ == '__main__':
    main()
