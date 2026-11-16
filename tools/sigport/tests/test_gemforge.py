# SPDX-License-Identifier: MIT
"""Read-only signature and relationship regression on the measured client (never executes it)."""
import copy
import hashlib
import json
import os
import struct
import subprocess
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sigdb_check import Resolver, parse
from sides import merged
from peimg import Image

EXE = os.environ.get('KILN_GEMFORGE_EXE')
ROOT = Path(__file__).resolve().parents[3]

@unittest.skipUnless(EXE, 'set KILN_GEMFORGE_EXE to the measured client file')
class GemForgeSignatures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = Image(EXE)
        cls.doc = merged(ROOT / 'sigdb/symbols/client.json')
        cls.resolved = Resolver(cls.image, cls.doc).run()

    def resolve(self, doc):
        return Resolver(self.image, doc).run()

    def test_measured_unique_symbols_and_relationships(self):
        symbols, constants, features = self.resolved
        self.assertTrue(features.get('ui.gemforge.inspect'))
        resolver = Resolver(self.image, self.doc)
        definitions = json.loads((ROOT / 'sigdb/symbols/client.d/gemforge.json').read_text())
        for name, definition in definitions['symbols'].items():
            for strategy in definition['find']:
                lo, hi = resolver.text_lo, resolver.text_hi
                if 'scope' in strategy:
                    lo = symbols[strategy['scope']['symbol']]['rva']
                    hi = min(hi, lo + strategy['scope']['size'])
                self.assertEqual(1, len(resolver.scan(parse(strategy['pattern'])[0], lo, hi)), name)
        for name, rva in {'GemForgeMenu': 0xcacb20, 'GemForgeLists': 0xcafac0,
                          'GemForgeCandidates': 0xd27cb0, 'GemForgeGrid': 0xcaefc0,
                          'GemForgeSourceRet': 0xcafbac, 'GemForgeSlotRet': 0xcaf59a,
                          'GemForgeScreenCall': 0xc61196, 'GemForgeListsCall': 0xcae1f4,
                          'GemForgeGemsCall': 0xcaff97, 'GemForgeWeaponsCall': 0xcaffd3,
                          'GemForgeEnd': 0xcaeece}.items():
            self.assertEqual(rva, symbols['ui/' + name]['rva'], name)
        for name, value in {'ui/gemforge.sockets': 0x6b40, 'ui/gemforge.socketCount': 0x6b48,
                            'ui/gemforge.selected': 0x10, 'ui/gemforge.weapon': 0x18}.items():
            self.assertEqual(value, constants[name]['value'], name)

    def test_hash_screen_switch_and_all_direct_call_edges(self):
        self.assertEqual('5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77',
                         hashlib.sha256(Path(EXE).read_bytes()).hexdigest())
        symbols, constants, _ = self.resolved
        def at(name): return symbols['ui/' + name]['rva']
        index = self.image.img[constants['ui/gemforge.switchMap']['value'] + 45 - 22]
        self.assertEqual(4, index)
        target = struct.unpack_from('<I', self.image.img, constants['ui/gemforge.switchTargets']['value'] + index * 4)[0]
        self.assertEqual(at('GemForgeScreenCall'), target)
        for site, callee, back in [('GemForgeScreenCall', 'GemForgeMenu', 0), ('GemForgeListsCall', 'GemForgeLists', 0),
                                   ('GemForgeGemsCall', 'GemForgeGrid', 0), ('GemForgeWeaponsCall', 'GemForgeGrid', 0),
                                   ('GemForgeSourceRet', 'GemForgeCandidates', 5), ('GemForgeSlotRet', 'ItemSlot', 5)]:
            address = at(site) - back
            self.assertEqual(0xe8, self.image.img[address])
            self.assertEqual(at(callee), address + 5 + struct.unpack_from('<i', self.image.img, address + 1)[0], site)

    @unittest.skipUnless(os.environ.get('KILN_FPTOOL'), 'set KILN_FPTOOL to verify the core ABI fingerprint')
    def test_core_fingerprint_matches_raw_hook_guard(self):
        request = Path(os.environ['KILN_GEMFORGE_SCRATCH']) / 'gemforge-fp-request.json'
        request.write_text(json.dumps({'rvas': [0xcaefc0]}))
        result = json.loads(subprocess.check_output([os.environ['KILN_FPTOOL'], EXE, str(request)], text=True))
        fp = result['rvas']['0xcaefc0']
        self.assertEqual('ok', fp['result'])
        self.assertEqual(['rcx', 'rdx', 'r8', 'r9'], fp['reads'])
        self.assertEqual(['xmm0', 'xmm2', 'xmm3'], fp['passes'])
        self.assertEqual(['0x30'], fp['stack'])
        self.assertEqual([], fp['indirect'])
        self.assertEqual('rax', fp['ret'])
        self.assertEqual(300, fp['insns'])
        self.assertTrue(fp['truncated'])

    def test_conflicting_call_edge_disables_feature(self):
        doc = copy.deepcopy(self.doc)
        edge = doc['symbols']['ui/GemForgeSourceRet']['find'][0]
        edge['pattern'] = edge['pattern'].replace('ui/GemForgeCandidates:rel32', 'ui/GemForgeMenu:rel32')
        _, _, features = self.resolve(doc)
        self.assertFalse(features.get('ui.gemforge.inspect'))

    def test_conflicting_layout_capture_disables_feature(self):
        doc = copy.deepcopy(self.doc)
        doc['constants']['ui/gemforge.sockets']['value'] = 0x6b50
        _, _, features = self.resolve(doc)
        self.assertFalse(features.get('ui.gemforge.inspect'))

if __name__ == '__main__':
    unittest.main()
