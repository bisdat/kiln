# SPDX-License-Identifier: MIT
"""Run with python3 -I tools/sfi/abigen/test_abigen.py (through the build slot)."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("sfi_abigen", HERE / "generate.py")
GEN = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GEN
SPEC.loader.exec_module(GEN)


class GeneratorTests(unittest.TestCase):
    def fixture(self):
        tmp = tempfile.TemporaryDirectory(prefix="sfi-abigen-test-")
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        abi = root / "native/core/abi"
        (abi / "services").mkdir(parents=True)
        (abi / "kiln_api.h").write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define KILN_CALL __cdecl
#define KILN_VERSION 3u
#define KILN_NAME "kiln.test"
#define KILN_MASK (1u << 31)
#define KILN_NEGATIVE (-1)
typedef uint64_t KilnOwner;
typedef struct KilnStr { const char *ptr; size_t len; } KilnStr;
typedef void(KILN_CALL* KilnFn)(const KilnStr *s, void *user);
enum { KILN_A = 4, KILN_B };
typedef struct KilnApi {
    uint32_t size, version;
    bool(KILN_CALL* enabled)(void);
    const void *(KILN_CALL* service)(KilnStr name, uint32_t min_version);
    void(KILN_CALL* subscribe)(KilnFn fn, void *user);
    const char *const *names;
    uint32_t values[3];
    union { uint64_t integer; double number; uint8_t bytes[17]; } payload;
} KilnApi;
''', encoding="utf-8")
        return root

    def test_types_constants_nullable_slots_and_union(self):
        result = GEN.generate(self.fixture(), "clang")
        text = result["src/bindings.rs"]
        self.assertIn('pub type KilnFn = Option<unsafe extern "C" fn(', text)
        self.assertIn('pub enabled: Option<unsafe extern "C" fn() -> bool>', text)
        self.assertIn('pub names: *const *const core::ffi::c_char', text)
        self.assertIn('pub values: [u32; 3]', text)
        self.assertIn('pub union KilnApi_payload', text)
        self.assertIn('pub const KILN_MASK: u32 = 2147483648;', text)
        self.assertIn('pub const KILN_NEGATIVE: i32 = -1;', text)
        self.assertIn('pub const KILN_B: i32 = 5;', text)
        self.assertIn('b"kiln.test\\0"', text)
        self.assertIn('offset_of!(crate::KilnApi, payload)', result['src/layout.rs'])
        self.assertIn('offset_of!(crate::KilnApi_payload, bytes)', result['src/layout.rs'])

    def test_determinism_and_no_absolute_paths(self):
        root = self.fixture()
        first = GEN.generate(root, "clang")
        self.assertEqual(first, GEN.generate(root, "clang"))
        for content in first.values():
            self.assertNotIn(str(root), content)

    def test_check_detects_input_and_output_drift(self):
        root = self.fixture()
        args = [sys.executable, "-I", str(HERE / "generate.py"), "--root", str(root)]
        subprocess.run(args, check=True, capture_output=True)
        self.assertEqual(subprocess.run(args + ['--check'], capture_output=True).returncode, 0)
        generated = root / 'crates/sfi/kiln-abi/src/bindings.rs'
        generated.write_text(generated.read_text() + '\n// drift\n')
        result = subprocess.run(args + ['--check'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('drift:', result.stderr)
        subprocess.run(args, check=True, capture_output=True)
        header = root / 'native/core/abi/kiln_api.h'
        header.write_text(header.read_text() + '\n// header drift\n')
        self.assertEqual(subprocess.run(args + ['--check'], capture_output=True).returncode, 1)

    def test_unsupported_layout_fails_closed(self):
        root = self.fixture()
        header = root / 'native/core/abi/kiln_api.h'
        header.write_text(header.read_text() + '\ntypedef struct KilnBits { unsigned flag:1; } KilnBits;\n')
        with self.assertRaisesRegex(GEN.GenerationError, 'bitfield'):
            GEN.generate(root, 'clang')

    def test_missing_header_fails_closed(self):
        root = self.fixture()
        (root / 'native/core/abi/kiln_api.h').unlink()
        with self.assertRaises(GEN.GenerationError):
            GEN.generate(root, 'clang')

    def test_unknown_macro_is_not_silently_omitted(self):
        root = self.fixture()
        header = root / 'native/core/abi/kiln_api.h'
        header.write_text(header.read_text() + '\n#define KILN_UNKNOWN ((void*)0)\n')
        with self.assertRaises(GEN.GenerationError):
            GEN.generate(root, 'clang')

    def test_by_value_enum_uses_c_enum_representation(self):
        root = self.fixture()
        header = root / 'native/core/abi/kiln_api.h'
        header.write_text(header.read_text() + '\ntypedef enum KilnChoice { KILN_CHOICE_A = -1, KILN_CHOICE_B = 2 } KilnChoice;\n')
        text = GEN.generate(root, 'clang')['src/bindings.rs']
        self.assertIn('pub type KilnChoice = i32;', text)

    def test_layout_metadata_does_not_collide_with_size_field(self):
        result = GEN.generate(self.fixture(), 'clang')
        self.assertIn('size_of::<crate::KilnStr>() == 16', result['src/layout.rs'])
        self.assertIn('size_of::<crate::KilnApi>() == 80', result['src/layout.rs'])
        self.assertIn('offset_of!(crate::KilnApi, size) == 0', result['src/layout.rs'])

    def test_service_collision_and_shared_type_identity(self):
        import json
        root = self.fixture()
        services = root / 'native/core/abi/services'
        for module, field in [('kiln_one', 'uint32_t value;'), ('kiln_two', 'uint64_t value;')]:
            (services / (module + '.h')).write_text('#include "../kiln_api.h"\ntypedef struct KilnCollision { ' + field + ' } KilnCollision;\n')
        result = GEN.generate(root, 'clang')
        manifest = json.loads(result['abi-manifest.json'])
        self.assertEqual(len(manifest['name_collisions']['KilnCollision']), 2)
        self.assertNotIn('pub struct KilnStr', result['src/services/kiln_one.rs'])
        self.assertIn('use crate::*;', result['src/services/kiln_one.rs'])

    def test_opaque_bytes_float_and_non_raw_self(self):
        root = self.fixture()
        header = root / 'native/core/abi/kiln_api.h'
        header.write_text(header.read_text() + '''
#define KILN_BYTES {'K', 'N', 1, 10}
#define KILN_DISTANCE 256.0
enum { KILN_HIGH_ENUM = 0x80000000u };
typedef struct KilnOpaque KilnOpaque;
typedef struct KilnHasSelf { uint32_t self; KilnOpaque *opaque; } KilnHasSelf;
''')
        result = GEN.generate(root, 'clang')
        text = result['src/bindings.rs']
        self.assertIn('pub const KILN_BYTES: [u8; 4] = [75, 78, 1, 10];', text)
        self.assertIn('pub const KILN_DISTANCE: f64 = 256.0;', text)
        self.assertIn('pub const KILN_HIGH_ENUM: i32 = -2147483648;', text)
        self.assertIn('pub struct KilnOpaque { _private: [u8; 0] }', text)
        self.assertIn('pub self_: u32,', text)
        self.assertIn('offset_of!(crate::KilnHasSelf, self_) == 0', result['src/layout.rs'])

    def test_float_macros_clang_prints_in_hex(self):
        # clang writes a float constant whose six-digit %e form would not give it back in hex (0x3F847AE140000000
        # for 0.01f): those must come out as the shortest literal that gives the same f32 / f64 again.
        root = self.fixture()
        header = root / 'native/core/abi/kiln_api.h'
        header.write_text(header.read_text() + '''
#define KILN_SMALL 0.01f
#define KILN_LARGE 100.0f
#define KILN_TENTH 0.1
#define KILN_THIRD (1.0 / 3.0)
#define KILN_NEGATIVE_SMALL (-0.3f)
''')
        text = GEN.generate(root, 'clang')['src/bindings.rs']
        self.assertIn('pub const KILN_SMALL: f32 = 0.01;', text)
        self.assertIn('pub const KILN_LARGE: f32 = 100.0;', text)
        self.assertIn('pub const KILN_TENTH: f64 = 0.1;', text)
        self.assertIn('pub const KILN_THIRD: f64 = 0.3333333333333333;', text)
        self.assertIn('pub const KILN_NEGATIVE_SMALL: f32 = -0.3;', text)

    def test_service_must_be_self_contained(self):
        root = self.fixture()
        (root / 'native/core/abi/services/kiln_bad.h').write_text('typedef struct KilnBad { KilnStr value; } KilnBad;\n')
        with self.assertRaisesRegex(GEN.GenerationError, 'unknown type name'):
            GEN.generate(root, 'clang')

    def test_strings_preserve_clang_bytes_and_exactly_one_terminator(self):
        root = self.fixture()
        header = root / 'native/core/abi/kiln_api.h'
        header.write_text(header.read_text() + r'''
#define KILN_ESCAPED "\xc3\xa9"
#define KILN_EMBEDDED "a\0b"
#define KILN_ADJACENT "a" "b"
''' + '#define KILN_SOURCE "é"\n')
        text = GEN.generate(root, 'clang')['src/bindings.rs']
        self.assertIn(r'pub const KILN_ESCAPED: &[u8] = b"\xc3\xa9\0";', text)
        self.assertIn(r'pub const KILN_SOURCE: &[u8] = b"\xc3\xa9\0";', text)
        self.assertIn(r'pub const KILN_EMBEDDED: &[u8] = b"a\0b\0";', text)
        self.assertIn(r'pub const KILN_ADJACENT: &[u8] = b"ab\0";', text)

    def test_only_active_owned_macros_are_emitted(self):
        import json
        root = self.fixture()
        header = root / 'native/core/abi/kiln_api.h'
        header.write_text(header.read_text() + '''
#if defined(_WIN32)
#define KILN_PLATFORM 1u
#define KILN_ACTIVE_HELPER(x) (x)
#else
#define KILN_PLATFORM 2u
#define KILN_PLATFORM_ONLY_NONWINDOWS 3u
#endif
#if 0
#define KILN_NEVER 4u
#define KILN_INACTIVE_HELPER(x) (x)
#endif
''')
        (root / 'native/core/abi/services/kiln_child.h').write_text('#include "../kiln_api.h"\n#define KILN_CHILD 9u\n')
        result = GEN.generate(root, 'clang')
        text = result['src/bindings.rs']
        self.assertEqual(text.count('pub const KILN_PLATFORM:'), 1)
        self.assertIn('pub const KILN_PLATFORM: u32 = 1;', text)
        self.assertNotIn('KILN_PLATFORM_ONLY_NONWINDOWS', text)
        self.assertNotIn('KILN_NEVER', text)
        self.assertNotIn('KILN_PLATFORM', result['src/services/kiln_child.rs'])
        inventory = json.loads(result['abi-manifest.json'])['headers']['native/core/abi/kiln_api.h']
        self.assertIn('KILN_ACTIVE_HELPER', inventory['function_macros'])
        self.assertNotIn('KILN_INACTIVE_HELPER', inventory['function_macros'])

    def test_manifest_hashes_exact_output_bytes_except_itself(self):
        import hashlib
        import json
        outputs = GEN.generate(self.fixture(), 'clang')
        manifest = json.loads(outputs['abi-manifest.json'])
        expected = {p: hashlib.sha256(text.encode('utf-8')).hexdigest() for p, text in outputs.items() if p != 'abi-manifest.json'}
        self.assertEqual(manifest['outputs'], expected)
        self.assertEqual(list(manifest['outputs']), sorted(manifest['outputs']))

    def test_llvm_literal_backslash_and_windows_path(self):
        root = self.fixture()
        header = root / 'native/core/abi/kiln_api.h'
        header.write_text(header.read_text() + r'''
#define KILN_PATH "C:\\mods\\"
#define KILN_SLASH "\\"
''')
        args = [sys.executable, '-I', str(HERE / 'generate.py'), '--root', str(root)]
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        text = (root / 'crates/sfi/kiln-abi/src/bindings.rs').read_text()
        self.assertIn(r'pub const KILN_PATH: &[u8] = b"C:\x5cmods\x5c\0";', text)
        self.assertIn(r'pub const KILN_SLASH: &[u8] = b"\x5c\0";', text)
        self.assertEqual(subprocess.run(args + ['--check'], capture_output=True).returncode, 0)

    def test_all_256_compiled_string_bytes_are_preserved(self):
        import ast
        import re
        root = self.fixture()
        header = root / 'native/core/abi/kiln_api.h'
        literal = ''.join('\\%03o' % byte for byte in range(256))
        header.write_text(header.read_text() + '\n#define KILN_ALL "' + literal + '"\n')
        text = GEN.generate(root, 'clang')['src/bindings.rs']
        literal = re.search(r'^pub const KILN_ALL: &\[u8\] = (b".*");$', text, re.M)[1]
        self.assertEqual(ast.literal_eval(literal), bytes(range(256)) + b'\0')


if __name__ == '__main__':
    unittest.main()
