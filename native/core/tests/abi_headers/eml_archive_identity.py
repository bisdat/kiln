#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reject copied/redeclared archive records anywhere in the public ABI headers."""
from pathlib import Path
import re
import unittest

ABI = Path(__file__).resolve().parents[2] / 'abi'
CANONICAL = ABI / 'services/kiln_eml_archive.h'

def definitions(text):
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
    return re.findall(r'typedef\s+struct\s+(\w+)\s*\{', text)

def duplicates(sources):
    canonical = set(definitions(sources[CANONICAL]))
    errors = []
    for path, text in sources.items():
        if path == CANONICAL:
            continue
        clean = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
        for name in canonical:
            # Even a forward declaration is a second independently maintained
            # archive declaration: consumers must include the canonical header.
            if re.search(r'\b(?:typedef\s+)?struct\s+' + name + r'\b', clean):
                errors.append((path, name))
    return errors

class CanonicalArchive(unittest.TestCase):
    def test_actual_public_headers_have_one_archive_definition(self):
        sources = {p: p.read_text() for p in ABI.rglob('*.h')}
        self.assertFalse(duplicates(sources))
        self.assertIn('KilnEmlArchiveIdentityV1', definitions(sources[CANONICAL]))

    def test_obsolete_values_copy_is_rejected(self):
        sources = {CANONICAL: CANONICAL.read_text(),
                   ABI / 'services/fake_values.h': 'typedef struct KilnEmlArchiveIdentityV1 { unsigned size; } KilnEmlArchiveIdentityV1;'}
        self.assertEqual(len(duplicates(sources)), 1)

    def test_forward_copy_is_rejected_but_canonical_include_is_not(self):
        path = ABI / 'services/fake_values.h'
        sources = {CANONICAL: CANONICAL.read_text(), path: 'typedef struct KilnEmlArchiveIdentityV1 KilnEmlArchiveIdentityV1;'}
        self.assertEqual(len(duplicates(sources)), 1)
        sources[path] = '#include "kiln_eml_archive.h"\n'
        self.assertFalse(duplicates(sources))

if __name__ == '__main__':
    unittest.main()
