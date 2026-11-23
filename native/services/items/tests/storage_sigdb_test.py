#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Qualify the player storage sigdb rows on read-only copies of the qualified exes; never starts the game.

    python3 -I native/services/items/tests/storage_sigdb_test.py --server <enshrouded_server.exe> --client <enshrouded.exe>

KILN_SERVER_EXE / KILN_CLIENT_EXE stand in for a missing --server / --client. The copies are read as files, never run,
and are never part of the repository. Without a copy that side is skipped, with a message, and the exit code is 0. A
copy from the environment that is another build is skipped too; a copy named on the command line must be the
qualified build (EXPECTED), or the test fails. native/services/items/tests/run.sh runs it after the Wine tests.
"""
import argparse
import copy
import hashlib
import json
import os
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

ENV = {'server': 'KILN_SERVER_EXE', 'client': 'KILN_CLIENT_EXE'}


def call_contracts(image, side, sha, symbols):
    """kiln#135: which pointer each structural storage call takes, read from the code. A World is what Kiln's
    server_world() returns; its entity storage (kiln.items' Access::storage) sits at world + world.storage."""
    build = json.loads((ROOT / 'sigdb/builds' / side / (sha + '.json')).read_text())
    world_storage = int(build['constants']['world.storage'], 16)
    entities = int(build['constants']['world/storage.entities'], 16)
    img = image.img
    u32 = lambda at: struct.unpack_from('<I', img, at)[0]
    rel = lambda at, size: at + size + struct.unpack_from('<i', img, at + size - 4)[0]
    # items/CreateEntity: mov r8, rdx; add rcx, world.storage; mov edx, [.rdata 0]; xor r9d, r9d; jmp spawn/CreateEntity
    ce = symbols['items/CreateEntity']['rva']
    check(img[ce:ce + 6] == bytes.fromhex('4C8BC24881C1') and u32(ce + 6) == world_storage,
          f'{side} items/CreateEntity adds world.storage to its first argument: it takes the World, not the storage')
    check(img[ce + 10:ce + 12] == bytes.fromhex('8B15') and u32(rel(ce + 10, 6)) == 0 and
          img[ce + 16:ce + 19] == bytes.fromhex('4533C9'), f'{side} items/CreateEntity requests id 0, no mask')
    check(img[ce + 19] == 0xE9 and rel(ce + 19, 5) == int(build['symbols']['spawn/CreateEntity'], 16),
          f'{side} items/CreateEntity tail-calls spawn/CreateEntity with the storage')
    # spawn/DestroyEntity (= items/StorageDestroyEntity) indexes the storage's entity map directly: it takes the storage
    de = symbols['items/StorageDestroyEntity']['rva']
    at = img.find(bytes.fromhex('488DB9'), de, de + 0x40)
    check(at > 0 and u32(at + 3) == entities, f'{side} items/StorageDestroyEntity takes the entity storage')
    # RecordFromStack finds a PIDE stack's row through EntityRef +8 at + world.storage + storage.entities: a World
    rfs = symbols['items/RecordFromStack']['rva']
    calls = [m for m in range(rfs, rfs + 0x600) if img[m:m + 6] == bytes.fromhex('498B4C2408E8')]
    check(calls and all(img[rel(c + 5, 5):rel(c + 5, 5) + 9] == bytes.fromhex('40534883EC20488D99') and
                        u32(rel(c + 5, 5) + 9) == world_storage + entities for c in calls),
          f'{side} EntityRef +8 is the World (RecordFromStack row lookup)')
    # PideFromRecord: r15 = its context (4th argument); every items/CreateEntity call takes rcx = [r15], context +0
    pfr = symbols['items/PideFromRecord']['rva']
    check(img.find(bytes.fromhex('4D8BF9'), pfr, pfr + 0x40) > 0, f'{side} PideFromRecord keeps its context in r15')
    creates = [c for c in range(pfr, pfr + 0x900) if img[c] == 0xE8 and rel(c, 5) == ce]
    check(len(creates) == 2 and all(img.find(bytes.fromhex('498B0F'), c - 0x40, c) > 0 for c in creates),
          f'{side} PideFromRecord hands its context +0 to items/CreateEntity: context +0 is the World')


def copies():
    """{side: (path, named on the command line)} of the exe copies that exist; the others are reported as skipped."""
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--server', help='read-only copy of enshrouded_server.exe (else $KILN_SERVER_EXE)')
    parser.add_argument('--client', help='read-only copy of enshrouded.exe (else $KILN_CLIENT_EXE)')
    args = parser.parse_args()
    found = {}
    for side, env in ENV.items():
        named = getattr(args, side)
        path = named or os.environ.get(env)
        if not path:
            print(f'SKIP {side}: no exe copy (--{side} or {env})', flush=True)
        elif not Path(path).is_file():
            if named:
                parser.error(f'--{side} {path}: no such file')
            print(f'SKIP {side}: {env}={path} is not a file', flush=True)
        else:
            found[side] = (Path(path), bool(named))
    return found


def main():
    found = copies()
    ran = []
    for side in EXPECTED:
        if side not in found:
            continue
        path, named = found[side]
        sha, addresses = EXPECTED[side]
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != sha and not named:
            print(f'SKIP {side}: {path} is build {actual[:12]}; these rows are qualified on {sha[:12]}', flush=True)
            continue
        ran.append(side)
        check(actual == sha, side + ' exact build identity')
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
        call_contracts(image, side, sha, symbols)
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
    if not ran:
        print('player storage sigdb: skipped, no qualified exe copies. Run it with --server <enshrouded_server.exe> '
              '--client <enshrouded.exe> (or KILN_SERVER_EXE / KILN_CLIENT_EXE): read-only copies, never run.')
        return
    skipped = [side for side in EXPECTED if side not in ran]
    print(f'player storage sigdb: {checks} passed, 0 failed' + (f'; {", ".join(skipped)} skipped' if skipped else ''))

if __name__ == '__main__':
    main()
