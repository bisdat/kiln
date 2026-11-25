#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""kiln.stations.client (0.2.0-dev.31.13): qualify stations.client_view on static copies of the pinned executables.

Read as files, never run. The client's ClientFactoryInfo cache (constructor, getter, row search, per-row decoder), the
HUD's remaining-time helper and its two call sites resolve once each on client 26430625 with every field offset
captured; the decoder's stream order is the server encoder's on both executables (the client's own copy of the server
code and the dedicated server's); the helper's arithmetic and constants are the ones client_stations_logic.h
re-implements; the decoder drops a row outside a known base; a changed byte turns the feature off; the dedicated
server has no such feature. docs/re/stations.md section 14.
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
    """The feature's file plus the shared names it lists (FactorySpeed and recipe.duration from server.d/stations.json's
    FactoryStationUpdate, content/RecipeFind from server.d/content.json), as the client composes them."""
    parts = {name: doc for name, doc, _ in compose('client')}
    own = parts['stations-client-view.json']
    doc = {'symbols': copy.deepcopy(own['symbols']), 'constants': copy.deepcopy(own['constants']),
           'features': copy.deepcopy(own['features'])}
    st = parts['stations.json']
    for name in ['stations/FactoryStationUpdate', '@stations/FactoryStationUpdate.timing', '@stations/FactorySpeed.fields']:
        doc['symbols'][name] = st['symbols'][name]
    for name in ['stations/recipe.duration']:
        if name in st.get('constants', {}):
            doc['constants'][name] = st['constants'][name]
    doc['symbols']['content/RecipeFind'] = parts['content.json']['symbols']['content/RecipeFind']
    return parts, doc


def rel(img, at):
    return at + 5 + struct.unpack_from('<i', img, at + 1)[0]


def rip_target(img, at, length, disp_at):
    """The absolute RVA a RIP-relative operand names: instruction at `at` of `length` bytes, disp32 at `disp_at`."""
    return at + length + struct.unpack_from('<i', img, disp_at)[0]


def decoder_order(img, start, size):
    """(offset, width) of every `lea rdx,[r15+d]` + `mov r8d,w` pair in the row decoder, in stream order; the two
    4-entry selection loops (`lea rbx,[r15+d]`, `mov edi,4` and an r8d width inside) as (d, w) x 4."""
    out = []
    body = img[start:start + size]
    i = 0
    while i < len(body) - 12:
        if body[i:i + 3] == b'\x49\x8d\x57' and body[i + 4:i + 6] == b'\x41\xb8':
            out.append((body[i + 3], struct.unpack_from('<I', body, i + 6)[0]))
            i += 10
            continue
        if body[i:i + 3] == b'\x49\x8d\x97' and body[i + 7:i + 9] == b'\x41\xb8':
            out.append((struct.unpack_from('<I', body, i + 3)[0], struct.unpack_from('<I', body, i + 9)[0]))
            i += 13
            continue
        if body[i:i + 3] == b'\x49\x8d\x5f' and body[i + 4:i + 9] == b'\xbf\x04\x00\x00\x00':
            w = re.search(rb'\x41\xb8(.)\x00\x00\x00', body[i + 9:i + 40], re.S)
            for k in range(4):
                out.append((body[i + 3] + 8 * k, w.group(1)[0]))
            i += 9
            continue
        i += 1
    return out


def main():
    a = argparse.ArgumentParser(description=__doc__)
    a.add_argument('--client', required=True)
    a.add_argument('--server', required=True)
    a = a.parse_args()
    client, server = Path(a.client), Path(a.server)
    check(hashlib.sha256(client.read_bytes()).hexdigest() == CLIENT_SHA, 'qualified client identity (Steam 26430625)')
    check(hashlib.sha256(server.read_bytes()).hexdigest() == SERVER_SHA, 'qualified dedicated identity (Steam 26430253)')
    parts, doc = definitions()
    check('stations-client-view.json' not in {n for n, _, _ in compose('server')}, 'the file is client-only (client.d)')
    c, s = Image(client), Image(server)
    found, consts, feats = Resolver(c, doc).run()
    check(feats['stations.client_view'], 'stations.client_view resolves on the pinned client')
    for name, rva in [('stations/client.FactoryInfoInit', 0x3bf5d0), ('stations/client.FactoryInfoGroups', 0xe869b0),
                      ('stations/client.FactoryInfoFind', 0x3bf700), ('stations/client.FactoryInfoRead', 0x3c0920),
                      ('stations/client.FactoryRemaining', 0xb2d6e0), ('stations/FactorySpeed', 0xe903d0),
                      ('stations/client.HudFactorySpeed', 0xe903d0), ('content/RecipeFind', 0xe997e0)]:
        check(found[name]['ok'] and found[name]['rva'] == rva, f'{name} = {rva:#x}')
    want = {'stations/cfi.size': 0x30f00, 'stations/cfi.count': 8, 'stations/cfi.capacity_at': 0x10,
            'stations/cfi.max_rows': 1000, 'stations/cfi.groups': 0x30d68, 'stations/cfi.group_count': 0x30d70,
            'stations/cfi.group_stride': 20, 'stations/cfi.row_size': 0xc8, 'stations/cfi.position': 0x0c,
            'stations/cfi.base': 0xb0, 'stations/cfi.missing': 0xa7, 'stations/cfi.state': 0xa6,
            'stations/cfi.water_max': 0xa4, 'stations/cfi.start': 0x28, 'stations/cfi.workshop': 4,
            'stations/cfi.recipe': 8, 'stations/cfi.sel_index': 0x34, 'stations/cfi.sel_category': 0x30,
            'stations/cfi.crafts': 0xa0, 'stations/cfi.water': 0xa8, 'stations/cfi.use_all': 0xaa,
            'stations/cfi.energy': 0xab, 'stations/cfi.recipe_duration': 0x38, 'stations/recipe.duration': 0x38,
            'stations/play.balancing': 0x4667d8, 'stations/play.clock': 0}
    for name, v in want.items():
        check(consts[name]['ok'] and consts[name]['value'] == v, f'{name} = {v:#x}')
    # the defaults of client_stations_logic.h's RowLayout are this build's
    logic = (ROOT / 'native/services/items/client_stations_logic.h').read_text()
    for field, name in [('size', 'row_size'), ('workshop', 'workshop'), ('recipe', 'recipe'), ('position', 'position'),
                        ('start', 'start'), ('sel_category', 'sel_category'), ('sel_index', 'sel_index'),
                        ('crafts', 'crafts'), ('water_max', 'water_max'), ('state', 'state'), ('missing', 'missing'),
                        ('water', 'water'), ('use_all', 'use_all'), ('energy', 'energy'), ('base', 'base')]:
        m = re.search(r'\b' + field + r' = (0x[0-9a-f]+)', logic)
        check(m and int(m.group(1), 16) == want['stations/cfi.' + name], f'RowLayout.{field} is the captured {name}')
    check(not Resolver(s, doc).run()[2]['stations.client_view'], 'no stations.client_view on the dedicated executable')

    img = c.img
    # the resource: rows inline at +0x28 behind a pointer at +0, so rows [0, count) are the cache's rows
    check(img[0x3bf60b:0x3bf616] == bytes.fromhex('4883C0284C894208488902'), 'constructor: rows pointer +0 = inline rows, count +8 = 0')
    check(struct.unpack_from('<I', img, 0x3bf5db)[0] == 0x30f00 and 0x28 + 1000 * 0xc8 == 0x30d68 and 0x30d90 + 16 * 20 <= 0x30f00,
          'the 0x30f00 bytes hold the 1000 inline rows up to the group vector at +0x30d68, then 16 inline groups')
    # decoder: field order and widths, equal to the server encoder's order (client copy and dedicated server)
    order = decoder_order(img, 0x3c0920, 0x2f3)
    expect = ([(0xa4, 2), (0x28, 8), (0x04, 4), (0x08, 4)] + [(0x34 + 8 * k, 1) for k in range(4)] +
              [(0x30 + 8 * k, 4) for k in range(4)] + [(0x78 + 4 * k, 4) for k in range(10)] +
              [(0x18 + 4 * k, 4) for k in range(4)] + [(0xac, 4), (0xa0, 4), (0xa8, 2)])
    check(order == expect, 'the row decoder reads the fields in the expected stream order and widths')
    check(img[0x3c09c0:0x3c09d9] == bytes.fromhex('0FB6442468498D97A7000000418887A600000041B801000000'),
          'the first stream byte is the state (+0xa6), the second (1 byte) the missing requirements (+0xa7)')
    enc = [bytes.fromhex(x) for x in ['0FB65F04', '0FB65F05', '0FB75F06', '488B5F30', '8B5F08', '8B5F0C']]
    tail = [bytes.fromhex(x) for x in ['8B9FAC000000', '0FB79FA4000000', '0FB69FA6000000', '0FB69FA7000000']]

    def in_order(im, seq, first):
        at = first
        for b in seq:
            at = im.img.find(b, at, at + 0x200)
            if at < 0:
                return False
            at += 1
        return True
    check(in_order(c, enc, 0x4a4913) and in_order(c, tail, 0x4a52f2),
          'client encoder (0x4a468c): state, missing, water max, start, workshop, recipe ... crafts, water, use-all, energy')
    check(in_order(s, enc, 0x23d1e3) and in_order(s, tail, 0x23dbc2),
          'dedicated encoder (0x23d1e3..): the same order: the server sends what the client decodes')
    # rows outside a base the client knows are never stored
    check(rel(img, 0x3bfbcf) == 0x46fd00 and img[0x3bfbd4:0x3bfbdb] == bytes.fromhex('807DB8000F8577'),
          'the decoder looks up the base by position and skips the row when none is found')
    check(rip_target(img, 0x3bf975, 9, 0x3bf975 + 5) == 0x13d290c and struct.unpack_from('<f', img, 0x13d290c)[0] == 0.25,
          'positions are u16 x 0.25 m')
    # the remaining-time helper: what client_stations_logic.h's game_remaining and scaled_ns re-implement
    check(struct.unpack_from('<d', img, rip_target(img, 0xb2d714, 8, 0xb2d718))[0] == 1e-9 and
          struct.unpack_from('<d', img, rip_target(img, 0xb2d727, 8, 0xb2d72b))[0] == 1e9,
          'one craft = float(ns * 1e-9) * speed, back to ns * 1e9')
    check(img[0xb2d74a:0xb2d74e] == bytes.fromhex('490FAFD3') and img[0xb2d7be:0xb2d7c1] == bytes.fromhex('4903D1') and
          img[0xb2d7e5:0xb2d7eb] == bytes.fromhex('488BC2492BC0'),
          'remaining = craft * crafts + craft - elapsed (imul, add, sub)')
    check(img[0xb2d6e7:0xb2d6f4] == bytes.fromhex('4885C975084' + '88BC14883C408C3'), 'no recipe: the helper answers 0 (Kiln: unknown)')
    # the HUD and the station screen show a time only for state 4 (Running), as the view does
    check(img[0xd9dac4:0xd9dacd] == bytes.fromhex('4180BC2EAE00000004'),
          'the HUD compares its row copy\'s state (+8 prefix, +0xae = +0xa6) with 4')
    check(img[0xbb84e3:0xbb84ea] == bytes.fromhex('0FB683A6000000') and img[0xbb8511:0xbb8513] == bytes.fromhex('3C04'),
          'the station screen shows the remaining time only while the state is 4 (Running)')
    # a changed byte turns the feature off (no silent misread on another build)
    for at, what in [(0x3c09d5, 'another width of the decoder\'s second read'), (0xb2d6f7, 'a crafts offset in the helper the decoder disagrees with'),
                     (0xd9db3e, 'a different clock read at the HUD site'), (0x3bf612, 'a moved count in the constructor'),
                     (0x3c0a15, 'another instruction than the stream read after the workshop field')]:
        altered = copy.copy(c)
        b = bytearray(img)
        b[at] ^= 0x01
        altered.img = bytes(b)
        check(not Resolver(altered, doc).run()[2]['stations.client_view'], f'{what} ({at:#x}) turns stations.client_view off')
    print(f'stations client view sigdb: {checks} passed, 0 failed')


if __name__ == '__main__':
    main()
