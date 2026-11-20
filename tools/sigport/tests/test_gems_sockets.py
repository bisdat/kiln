# SPDX-License-Identifier: MIT
"""gems.sockets on the measured client (sigdb/symbols/client.d/gems-sockets.json): read-only, never executes it."""
import copy
import hashlib
import json
import os
import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sigdb_check import Resolver, parse
from sides import merged, compose
from peimg import Image

EXE = os.environ.get('KILN_GEMFORGE_EXE')
ROOT = Path(__file__).resolve().parents[3]
SHA = '5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77'


class Composition(unittest.TestCase):
    def test_client_file_extends_the_servers_and_keeps_its_symbols(self):
        parts = {name: (doc, src) for name, doc, src in compose('client', ROOT / 'sigdb/symbols')}
        doc, src = parts['gems-sockets.json']
        self.assertIn('over server.d/gems-sockets.json', src)
        server = json.loads((ROOT / 'sigdb/symbols/server.d/gems-sockets.json').read_text())
        feature = doc['features']['gems.sockets']
        # Everything the server's row needs is part of the client's row too (the client hosts the same createPide).
        for name in server['features']['gems.sockets']['symbols']:
            self.assertIn(name, feature['symbols'])
        for name in server['features']['gems.sockets']['constants']:
            self.assertIn(name, feature['constants'])
        for name in server['symbols']:
            self.assertIn(name, doc['symbols'])
        for name in ('ui/GemForgeLists', 'ui/GemForgeCandidates', 'ui/GemForgeWeaponsMask', 'ui/GemForgeCarrierMask'):
            self.assertIn(name, feature['symbols'])


@unittest.skipUnless(EXE, 'set KILN_GEMFORGE_EXE to the measured client file')
class GemsSocketsSignatures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = Image(EXE)
        cls.doc = merged(ROOT / 'sigdb/symbols/client.json')
        cls.resolved = Resolver(cls.image, cls.doc).run()

    def test_hash(self):
        self.assertEqual(SHA, hashlib.sha256(Path(EXE).read_bytes()).hexdigest())

    def test_unique_scoped_patterns_sites_and_captures(self):
        symbols, constants, features = self.resolved
        self.assertTrue(features.get('gems.sockets'))
        resolver = Resolver(self.image, self.doc)
        definitions = json.loads((ROOT / 'sigdb/symbols/client.d/gems-sockets.json').read_text())
        for name, definition in definitions['symbols'].items():
            for strategy in definition['find']:
                lo = symbols[strategy['scope']['symbol']]['rva']
                hi = min(resolver.text_hi, lo + strategy['scope']['size'])
                rx = parse(strategy['pattern'])[0]
                self.assertEqual(1, len(resolver.scan(rx, lo, hi)), name + ' in its scope')
                # Unique in the whole .text too, not only in the measured scope.
                self.assertEqual(1, len(resolver.scan(rx, resolver.text_lo, resolver.text_hi, limit=3)), name + ' image-wide')
        self.assertEqual(0xcafc89, symbols['ui/GemForgeWeaponsMask']['rva'])
        self.assertEqual(0xd27d80, symbols['ui/GemForgeCarrierMask']['rva'])
        self.assertEqual(0x6c, constants['ui/gemforge.weaponsMask']['value'])
        self.assertEqual(0x4c, constants['ui/gemforge.carrierMask']['value'])

    def test_hooked_instruction_follows_the_weapons_mask_store(self):
        symbols, constants, _ = self.resolved
        img = self.image.img
        for site, disp, flag in (('ui/GemForgeWeaponsMask', 'ui/gemforge.weaponsMask', 0x70),
                                 ('ui/GemForgeCarrierMask', 'ui/gemforge.carrierMask', 0x50)):
            at = symbols[site]['rva']
            d = constants[disp]['value']
            # mov dword [rsp+d], 4 ends exactly at the hook; the hook covers mov byte [rsp+flag], 1 (5 bytes, no rel).
            self.assertEqual(bytes([0xc7, 0x44, 0x24, d]) + struct.pack('<I', 4), bytes(img[at - 8:at]), site)
            self.assertEqual(bytes([0xc6, 0x44, 0x24, flag, 0x01]), bytes(img[at:at + 5]), site)

    def test_loose_gem_pass_is_not_a_site(self):
        symbols, _, _ = self.resolved
        img = self.image.img
        # The loose WeaponGem pass (mask 0x8000) at 0xd27e31 stays the game's.
        self.assertEqual(bytes.fromhex('c7 44 24 4c 00 80 00 00'.replace(' ', '')), bytes(img[0xd27e31:0xd27e39]))
        self.assertNotEqual(0xd27e39, symbols['ui/GemForgeCarrierMask']['rva'])

    def test_conflicting_capture_or_scope_disables_the_feature(self):
        doc = copy.deepcopy(self.doc)
        doc['constants']['ui/gemforge.weaponsMask']['value'] = 0x70
        self.assertFalse(Resolver(self.image, doc).run()[2].get('gems.sockets'))
        doc = copy.deepcopy(self.doc)
        doc['symbols']['ui/GemForgeCarrierMask']['find'][0]['scope']['symbol'] = 'ui/GemForgeLists'
        self.assertFalse(Resolver(self.image, doc).run()[2].get('gems.sockets'))


if __name__ == '__main__':
    unittest.main()
