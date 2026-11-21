#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The values header is its reviewed text (pinned by sha256), includes the canonical archive header,
and defines no name another public ABI header already defines."""
from pathlib import Path
import hashlib
import re
import unittest

ABI = Path(__file__).resolve().parents[2] / 'abi'
VALUES = ABI / 'services/kiln_eml_values.h'
ARCHIVE = ABI / 'services/kiln_eml_archive.h'
# The reviewed text of the header without its note and with the draft's include path. A change of the header's
# declarations or comments is a reviewed change of this pin.
DRAFT_SHA256 = 'a4ebd80df1b545b9f38f561efdac84b5d892b252f30b450cae8bb7c59be13f7c'
# The two edits the header carries beyond the reviewed draft: the Kiln-internal note and the bare include.
PRIVATE_NOTE = (' *\n'
                ' * Kiln-internal: the typed-value seam of Kiln\'s interpreter for ShroudForge mods, not in\n'
                ' * the SDK\'s headers and no mod API; making it public is a decision of its own.\n')
PRODUCT_INCLUDE = '#include "kiln_eml_archive.h"\n'
DRAFT_INCLUDE = '#include "services/kiln_eml_archive.h"\n'

def as_draft(text):
    if text.count(PRIVATE_NOTE) != 1 or text.count(PRODUCT_INCLUDE) != 1:
        return None
    return text.replace(PRIVATE_NOTE, '', 1).replace(PRODUCT_INCLUDE, DRAFT_INCLUDE, 1)

def strip_comments(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)

def defined_names(text):
    clean = strip_comments(text)
    names = set(re.findall(r'^\s*#\s*define\s+(\w+)', clean, re.M))
    names |= set(re.findall(r'typedef\s+struct\s+(\w+)', clean))
    names |= set(re.findall(r'}\s*(\w+)\s*;', clean))
    for body in re.findall(r'\benum\b[^{;]*\{(.*?)\}', clean, re.S):
        names |= set(re.findall(r'(\w+)\s*(?:=[^,]*)?(?:,|$)', body.strip()))
    return {n for n in names if n and not n.isdigit()}

# The include guard and the #undef'd assert helper are global macros too: a second header
# defining the guard would silently empty this one, and one defining the helper would lose it
# to the #undef. Both stay in the collision set (draft names, kept for the sha pin).
GUARD = 'SFI_TYPED_CONTAINER_DRAFT_H'
HELPER = 'SFI_VALUE_ASSERT'
NATIVE_ROOTS = [ABI.parents[1], ABI.parents[2] / 'gpl']  # native/ and gpl/

class ValuesHeader(unittest.TestCase):
    def test_header_is_the_assented_draft(self):
        draft = as_draft(VALUES.read_text())
        self.assertIsNotNone(draft, 'private note or bare include missing or repeated')
        self.assertEqual(hashlib.sha256(draft.encode()).hexdigest(), DRAFT_SHA256)

    def test_any_other_edit_breaks_the_pin(self):
        text = VALUES.read_text().replace('KilnHandle item, payload, error;', 'KilnHandle item, error, payload;')
        self.assertNotEqual(hashlib.sha256(as_draft(text).encode()).hexdigest(), DRAFT_SHA256)

    def test_includes_the_canonical_archive_header(self):
        includes = re.findall(r'^\s*#\s*include\s+"([^"]+)"', VALUES.read_text(), re.M)
        self.assertEqual(includes, ['kiln_eml_archive.h'])
        self.assertTrue((VALUES.parent / includes[0]).is_file())

    def test_no_name_collides_with_another_public_header(self):
        own = defined_names(VALUES.read_text())
        self.assertIn(GUARD, own)
        self.assertIn(HELPER, own)
        self.assertIn('KilnEmlValuesApiV1', own)
        self.assertIn('KILN_LUA_MAKE_TYPED_PROXY', own)
        self.assertIn('KILN_VALUE_ARRAY_MAPPED', own)
        clashes = []
        for path in ABI.rglob('*.h*'):
            if path != VALUES:
                clashes += [(path.name, n) for n in own & defined_names(path.read_text())]
        self.assertFalse(clashes)

    def test_guard_and_helper_are_unique_in_every_native_header(self):
        users = []
        for root in NATIVE_ROOTS:
            for path in root.rglob('*'):
                if path.suffix in ('.h', '.hpp', '.hh', '.inl') and path != VALUES and path.is_file():
                    text = path.read_text(errors='replace')
                    users += [(str(path), n) for n in (GUARD, HELPER) if re.search(r'\b' + n + r'\b', text)]
        self.assertFalse(users)

    def test_collision_check_sees_a_guard_clash(self):
        fake = '#ifndef SFI_TYPED_CONTAINER_DRAFT_H\n#define SFI_TYPED_CONTAINER_DRAFT_H\n#endif\n'
        self.assertEqual(defined_names(VALUES.read_text()) & defined_names(fake), {GUARD})

    def test_collision_check_sees_a_real_clash(self):
        fake = 'enum { KILN_VALUE_NIL = 1 };\ntypedef struct KilnEmlValueResultV1 { int x; } KilnEmlValueResultV1;'
        own = defined_names(VALUES.read_text())
        self.assertEqual(own & defined_names(fake), {'KILN_VALUE_NIL', 'KilnEmlValueResultV1'})

    def test_archive_records_are_not_redeclared(self):
        clean = strip_comments(VALUES.read_text())
        for name in re.findall(r'typedef\s+struct\s+(\w+)\s*\{', strip_comments(ARCHIVE.read_text())):
            self.assertNotRegex(clean, r'\bstruct\s+' + name + r'\b')

if __name__ == '__main__':
    unittest.main()
