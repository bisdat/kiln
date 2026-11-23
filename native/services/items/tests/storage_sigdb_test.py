#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Qualify storage signatures on explicit read-only exe copies; never starts the game."""
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
from sigdb_check import Resolver, parse

NAMES = ['characters/Capture', 'characters/Read', 'characters/JoinApply', 'items/RecordFromStack',
         'items/RecordWrite', 'items/RecordRead', 'items/PideFromRecord', 'items/CreateEntity',
         'saves/EcsSnapshot', 'items/TombstoneCheck', 'characters/Write',
         'saves/CapturesDone', 'saves/PublishCharacters']
EXPECTED = {
    'server': ('f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2',
               [0x72b440, 0x723a30, 0x7a93a0, 0x72a9b0, 0x741dc0, 0x71bd50, 0x7abd60, 0x620ce0, 0x62df70, 0x9471c0, 0x743d00, 0x71afd0, 0x73b750]),
    'client': ('5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77',
               [0xe19e60, 0xe0f4f0, 0xe4d850, 0xe193d0, 0xe353c0, 0xe071b0, 0xe50160, 0x984e50, 0x9920e0, 0xe9ff80, 0xe37300, 0xe06430, 0xe2d920])}
FIELDS = {'characters/struct.main_pages': 0x1e38, 'characters/struct.extra_pages': 0x1e44,
          'characters/struct.list': 0x1e50, 'characters/struct.equip0': 0x1a68,
          'characters/struct.equip1': 0x1aa4, 'characters/struct.equip_last': 0x1dec,
          'characters/struct.refs': 0x1e28, 'items/record.stride': 0x38}
checks = 0

def check(ok, text):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(text)
    print('PASS ' + text, flush=True)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True)
    parser.add_argument('--client', required=True)
    args = parser.parse_args()
    for side in EXPECTED:
        path = getattr(args, side)
        sha, addresses = EXPECTED[side]
        check(hashlib.sha256(Path(path).read_bytes()).hexdigest() == sha, side + ' exact build identity')
        doc = next(d for name, d, _ in compose(side) if name == 'player-storage.json')
        image = Image(path)
        symbols, constants, features = Resolver(image, doc).run()
        check(features.get('items.storage'), side + ' complete storage feature')
        fixture = json.loads((ROOT / 'native/services/items/tests/storage-resolve-builds.json').read_text())[side]
        for name, rva in zip(NAMES, addresses):
            check(int(fixture['symbols'][name], 16) == rva, f'{side} base address fixture {name}')
        for name, value in fixture['symbols'].items():
            rva = int(value, 16)
            check(symbols.get(name, {}).get('rva') == rva, f'{side} {name} = {rva:#x}')
        for name, value in fixture['constants'].items():
            check(constants.get(name, {}).get('ok') and constants[name]['value'] == value, f'{side} qualified {name} = {value:#x}')
        for name, value in FIELDS.items():
            check(constants.get(name, {}).get('ok') and constants[name]['value'] == value, f'{side} {name} = {value:#x}')
        stride = constants['characters/struct.equip1']['value'] - constants['characters/struct.equip0']['value']
        check(stride == 0x3c and constants['characters/struct.equip_last']['value'] ==
              constants['characters/struct.equip0']['value'] + 15 * stride, side + ' derived equipment stride and endpoint')
        # Every required entry, probe and constant is independently necessary, not advisory.
        feature = doc['features']['items.storage']
        for section, key in [('symbols', n) for n in feature['symbols']] + [('constants', n) for n in feature['constants']]:
            changed = copy.deepcopy(doc)
            if section == 'symbols' and key not in changed[section]:
                for definition in changed[section].values():
                    for strategy in definition.get('find', []):
                        strategy['pattern'] = strategy['pattern'].replace('{' + key + ':', '{omitted:')
            elif section == 'symbols':
                changed[section][key] = {'find': [{'pattern': 'DE AD BE EF 01 23 45 67 89 AB CD EF 98 76 54 32'}]}
            else:
                changed[section][key]['min_votes'] = 10000
            check(not Resolver(image, changed).run()[2].get('items.storage'), f'{side} missing {key} disables storage')
        # Reader and writer must contribute independently and agree. Change a captured writer hole in image bytes.
        for name in FIELDS:
            hits = []
            for probe, definition in doc['symbols'].items():
                for strategy in definition.get('find', []):
                    rx, holes, _ = parse(strategy['pattern'])
                    if not any(h[0] == name for h in holes):
                        continue
                    scope = strategy.get('scope')
                    check(scope is not None, side + ' field capture is function-scoped')
                    lo = symbols[scope['symbol']]['rva']
                    at = Resolver(image, {}).scan(rx, lo, lo + scope['size'])
                    check(len(at) == 1, f'{side} {probe} unique capture')
                    hits.append((scope['symbol'], at[0], holes))
            check({h[0] for h in hits} == {'characters/Read', 'characters/Write'}, f'{side} {name} independently read and written')
            _, at, holes = next(h for h in hits if h[0] == 'characters/Write')
            raw = bytearray(image.img)
            offset = next(o for n, _, o in holes if n == name)
            raw[at + offset] ^= 1
            altered = copy.copy(image)
            altered.img = bytes(raw)
            check(not Resolver(altered, doc).run()[2]['items.storage'], f'{side} disagreeing {name} disables storage')
    print(f'player storage sigdb: {checks} passed, 0 failed')

if __name__ == '__main__':
    main()
