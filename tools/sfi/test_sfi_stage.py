#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/sfi/sfi_stage.py and its wiring in tools/package/stage.sh : Kiln's interpreter
(kiln-sfi) is its own default component sfi on both sides, never swept into core or the hosted ShroudForge folder;
kiln-eml's archive inspector is component eml's, one folder per platform with its licence, notice, schema and a
version.json that binds its version and SHA-256; the hosted ShroudForge component stays the default; a developer test
build is refused on every branch. Synthetic stages in a temporary folder; no build, nothing run from the stage.
    python3 -I tools/sfi/test_sfi_stage.py"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


ss = load("sfi_stage", HERE / "sfi_stage.py")
pc = load("public_cut", HERE / "public_cut.py")
pl = load("plugins", REPO / "tools/release/lib/plugins.py")

BOTH = ["server", "client"]
COMPONENTS = {
    "core": {"sides": BOTH, "default": True},
    "eml": {"sides": BOTH, "default": True, "requires": ["core"]},
    "shroudforge": {"sides": BOTH, "default": True, "requires": ["core"]},
    "sfi": {"sides": BOTH, "default": True, "requires": ["core", "eml"]},
}


def pe(dll, stamp=None, extra=b"", code=None):
    """A minimal AMD64 PE32+ image: a DLL or an executable. With `stamp`, one section holding a kiln_build_stamp export
    compiled as both builds compile it ("lea rax, [rip+X]; ret", X the NUL-terminated stamp; `code` replaces those
    bytes); `extra` is appended (e.g. the core's own build metadata)."""
    b = bytearray(0x200)
    b[:2] = b"MZ"
    b[0x3C:0x40] = (0x80).to_bytes(4, "little")
    b[0x80:0x84] = b"PE\0\0"
    b[0x84:0x86] = (0x8664).to_bytes(2, "little")
    b[0x96:0x98] = ((0x2000 if dll else 0) | 0x0002 | 0x0020).to_bytes(2, "little")
    b[0x98:0x9A] = (0x20B).to_bytes(2, "little")
    if stamp is not None:
        le = lambda v, n: v.to_bytes(n, "little", signed=v < 0)
        b[0x86:0x88] = le(1, 2)  # one section
        b[0x94:0x96] = le(0xF0, 2)  # optional header: 112 bytes + 16 data directories
        b[0x98 + 108:0x98 + 112] = le(16, 4)
        b[0x98 + 112:0x98 + 120] = le(0x1000, 4) + le(40, 4)  # the export directory
        sec = bytearray(0x100)
        sec[20:40] = le(1, 4) + le(1, 4) + le(0x1040, 4) + le(0x1044, 4) + le(0x1048, 4)
        sec[0x40:0x4A] = le(0x1070, 4) + le(0x1050, 4) + le(0, 2)
        sec[0x50:0x61] = ss.STAMP_EXPORT + b"\0"
        sec[0x70:0x78] = code if code is not None else b"\x48\x8d\x05" + le(0x1080 - 0x1077, 4) + b"\xc3"
        sec[0x80:0x80 + len(stamp) + 1] = stamp.encode() + b"\0"
        b[0x188:0x1B0] = b".rdata\0\0" + le(len(sec), 4) + le(0x1000, 4) + le(len(sec), 4) + le(0x200, 4) + bytes(16)
        b += sec
    return bytes(b) + extra


VERSION = ss.kiln_version(str(REPO))
META = "+19700102.abc1234"
STAMP = VERSION + META


def core_dll(meta=META):
    """kiln-core.dll: no export, its build metadata among its strings (boot.cpp, api.cpp)."""
    return pe(True, extra=b"\0resourceId\0\0" + meta.encode() + b"\0")


ELF = b"\x7fELF\x02\x01\x01" + b"\0" * 9 + (3).to_bytes(2, "little") + (0x3E).to_bytes(2, "little") + b"\0" * 44


class Stage(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="kiln-sfi-stage-test-")
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def put(self, rel, data, mode=0o644):
        p = self.stage / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        os.chmod(p, mode)
        return p

    def make(self, components=None):
        """A complete stage with the sfi component, as stage.sh writes it."""
        self.stage = self.dir / "stage"
        shutil.rmtree(self.stage, ignore_errors=True)
        self.put("core/kiln/kiln-core.dll", core_dll())
        self.put("core/kiln/services/kiln-engine.dll", pe(True, STAMP))
        self.put("core/kiln/services/kiln-world.dll", pe(True, STAMP))
        self.put("core/kiln/plugins/kiln-shroudforge.dll", pe(True, STAMP))
        self.put("eml/kiln/plugins/kiln-eml.dll", pe(True))
        self.put("shroudforge/kiln/plugins/shroudforge/shroudforge-runtime.dll", pe(True))
        self.put("sfi/kiln/plugins/kiln-sfi.dll", pe(True, STAMP))
        for platform, exe in ss.PLATFORMS.items():
            d = f"eml/{ss.HELPER_DIR}/{platform}"
            if platform.startswith("linux"):
                self.put(f"{d}/{exe}", ELF, 0o755)
            else:
                self.put(f"{d}/{exe}", pe(False))
            self.put(f"{d}/LICENSE.txt", (REPO / ss.LICENSE_SRC).read_bytes())
            self.put(f"{d}/NOTICE.txt", (REPO / ss.NOTICE_SRC).read_bytes())
            self.put(f"{d}/{ss.SCHEMA}", (REPO / ss.SCHEMA_SRC).read_bytes())
            ss.write_version(str(REPO), str(self.stage / d), platform)
        (self.stage / "kiln-runtime.json").write_text(json.dumps({"format": 1, "components": components or COMPONENTS}))
        return self.stage

    def problems(self):
        return ss.check(str(REPO), str(self.stage))

    def assertProblem(self, text):
        bad = self.problems()
        self.assertTrue(any(text in p for p in bad), f"{text!r} not in {bad}")

    def comps(self, change):
        c = json.loads(json.dumps(COMPONENTS))
        change(c)
        (self.stage / "kiln-runtime.json").write_text(json.dumps({"format": 1, "components": c}))

    def version(self, platform, change):
        p = self.stage / "eml" / ss.HELPER_DIR / platform / "version.json"
        v = json.loads(p.read_text())
        change(v)
        p.write_text(json.dumps(v))

    def test_a_complete_stage_passes(self):
        self.make()
        self.assertEqual(self.problems(), [])
        self.assertEqual(ss.main(["sfi_stage.py", "check", str(REPO), str(self.stage)]), 0)
        v = json.loads((self.stage / "eml" / ss.HELPER_DIR / "linux-x86_64" / "version.json").read_text())
        self.assertEqual(v["version"], ss.helper_version(str(REPO)))
        self.assertEqual(v["sha256"], hashlib.sha256(ELF).hexdigest())
        self.assertEqual(v["framing"], "kiln-eml-inspect archive-set v1")
        # the version its answers name: kiln-eml's own (crate::VERSION = CARGO_PKG_VERSION)
        cargo = (REPO / ss.CARGO).read_text()
        self.assertIn(f'version = "{v["version"]}"', cargo)

    def test_the_sfi_component_missing_empty_or_not_a_dll(self):
        self.make()
        (self.stage / "sfi/kiln/plugins/kiln-sfi.dll").unlink()
        self.assertProblem("kiln/plugins/kiln-sfi.dll: missing")
        self.put("sfi/kiln/plugins/kiln-sfi.dll", b"")
        self.assertProblem("kiln/plugins/kiln-sfi.dll: empty")
        self.put("sfi/kiln/plugins/kiln-sfi.dll", pe(False))
        self.assertProblem("not an AMD64 PE32+ DLL")
        self.make()
        self.comps(lambda c: c.pop("sfi"))
        self.assertProblem("component sfi is not in the manifest")

    def test_the_sfi_component_one_sided_non_default_or_without_its_provider(self):
        self.make()
        self.comps(lambda c: c["sfi"].update(sides=["client"]))
        self.assertProblem("component sfi is not for server")
        self.comps(lambda c: c["sfi"].update(default=False))
        self.assertProblem("component sfi is not installed by default")
        self.comps(lambda c: c["sfi"].update(requires=["core"]))
        self.assertProblem("component sfi does not require eml")
        self.comps(lambda c: c.pop("eml"))
        self.assertProblem("requires eml, which the manifest does not have")

    def test_kiln_sfi_carries_the_core_build_stamp(self):
        """The 0.2.0-dev.30.2/30.3 bug: kiln-sfi.dll without the core's stamp, so the core never designated it."""
        self.make()
        self.assertEqual(ss.stamp_problems(str(REPO), str(self.stage)), [])
        self.assertEqual(ss.pe_build_stamp((self.stage / "sfi" / ss.SFI_DLL).read_bytes()), STAMP)
        for case, dll, text in (
                ("no export (as released)", pe(True), "exports no kiln_build_stamp, so the core never makes it the "
                                                      "ShroudForge-dialect owner"),
                ("another build", pe(True, VERSION + "+19700102.abc1235"), f"is not the core's {STAMP}"),
                ("a build without metadata", pe(True, VERSION), f"its build stamp {VERSION} is not the core's"),
                ("another version", pe(True, "0.0.1" + META), "is not the core's"),
                ("unreadable code", pe(True, STAMP, code=b"\x48\x8b\x05\x09\0\0\0\xc3"), "cannot be read"),
                ("a forwarder", None, "cannot be read")):
            with self.subTest(case=case):
                self.make()
                if dll is None:  # the export's RVA inside the export directory: a forwarder string
                    data = bytearray(pe(True, STAMP))
                    data[0x240:0x244] = (0x1010).to_bytes(4, "little")
                    dll = bytes(data)
                self.put("sfi/kiln/plugins/kiln-sfi.dll", dll)
                self.assertProblem(text)
                self.assertEqual(ss.main(["sfi_stage.py", "check", str(REPO), str(self.stage)]), 1)

    def test_the_core_build_is_one_stamped_build(self):
        self.make()
        self.put("core/kiln/services/kiln-world.dll", pe(True, VERSION + "+19700102.abc1235"))
        self.assertProblem("the service DLLs are of different builds")
        self.make()
        self.put("core/kiln/services/kiln-world.dll", pe(True))
        self.assertProblem("kiln-world.dll: exports no kiln_build_stamp (every native service DLL does)")
        self.make()
        for name in ("kiln-engine.dll", "kiln-world.dll"):
            self.put(f"core/kiln/services/{name}", pe(True, VERSION))
        self.put("sfi/kiln/plugins/kiln-sfi.dll", pe(True, VERSION))
        self.assertProblem("a build without build metadata: the core trusts no DLL")
        self.make()
        self.put("core/kiln/kiln-core.dll", core_dll("+19700102.abc1235"))
        self.assertProblem("does not hold the build metadata +19700102.abc1234 of its services")
        self.make()
        shutil.rmtree(self.stage / "core" / "kiln" / "services")
        self.assertProblem("no native service DLL with a build stamp")
        self.make()
        (self.stage / "core" / ss.CORE_DLL).unlink()
        self.assertProblem("kiln-core.dll: missing")

    def test_the_stamp_reader(self):
        self.assertIsNone(ss.pe_build_stamp(pe(True)))
        self.assertEqual(ss.pe_build_stamp(pe(True, "0.2.0-dev+nogit.0123456789abcdef")), "0.2.0-dev+nogit.0123456789abcdef")
        # an incremental-link thunk: jmp rel32 to the real code
        thunk = bytearray(pe(True, STAMP, code=b"\xe9" + (0x1078 - 0x1075).to_bytes(4, "little") + b"\xcc\xcc\xcc"))
        thunk[0x278:0x280] = b"\x48\x8d\x05" + (0x1080 - 0x107F).to_bytes(4, "little") + b"\xc3"
        thunk[0x280:0x280 + len(STAMP) + 1] = STAMP.encode() + b"\0"
        self.assertEqual(ss.pe_build_stamp(bytes(thunk)), STAMP)
        for bad in (b"not a PE", pe(True, "x" * 129), pe(True, "bad\tstamp")):
            with self.assertRaises(ValueError):
                ss.pe_build_stamp(bad)
        p = self.dir / "kiln-sfi.dll"
        p.write_bytes(pe(True, STAMP))
        self.assertEqual(ss.main(["sfi_stage.py", "stamp", str(p)]), 0)
        p.write_bytes(pe(True))
        self.assertEqual(ss.main(["sfi_stage.py", "stamp", str(p)]), 1)

    def test_the_interpreter_never_lands_in_core_or_the_hosted_folder(self):
        for rel in ("core/kiln/plugins/kiln-sfi.dll", "shroudforge/kiln/plugins/shroudforge/kiln-sfi.dll",
                    "eml/kiln/plugins/kiln-sfi.pdb", "core/kiln/plugins/KILN-SFI.dll"):
            with self.subTest(rel=rel):
                self.make()
                self.put(rel, pe(True))
                self.assertProblem("kiln-sfi (Kiln's interpreter) outside its component")
        # anything else in the MIT component: refused (the GPL helper above all)
        self.make()
        self.put(f"sfi/{ss.HELPER_DIR}/linux-x86_64/kiln-eml-inspect", ELF, 0o755)
        self.assertProblem("component sfi holds only kiln/plugins/kiln-sfi.dll")
        self.assertProblem("belongs to component eml (GPL-3)")

    def test_the_hosted_shroudforge_component_stays_the_default(self):
        self.make()
        shutil.rmtree(self.stage / "shroudforge")
        self.comps(lambda c: c.pop("shroudforge"))
        self.assertProblem("component shroudforge (ShroudForge's runtime, the default backend) is missing")
        self.make()
        self.comps(lambda c: c["shroudforge"].update(default=False))
        self.assertProblem("is not installed by default")

    def test_each_helper_is_complete_and_bound_by_its_version_file(self):
        for platform, exe in ss.PLATFORMS.items():
            d = f"eml/{ss.HELPER_DIR}/{platform}"
            with self.subTest(platform=platform, case="missing"):
                self.make()
                (self.stage / d / exe).unlink()
                self.assertProblem(f"{d}/{exe}: missing")
            with self.subTest(platform=platform, case="forged hash"):
                self.make()
                self.version(platform, lambda v: v.update(sha256="0" * 64))
                self.assertProblem("version.json: sha256")
            with self.subTest(platform=platform, case="swapped helper"):
                self.make()
                p = self.stage / d / exe
                p.write_bytes(p.read_bytes() + b"swapped")
                self.assertProblem("version.json: sha256")
            with self.subTest(platform=platform, case="stale version"):
                self.make()
                self.version(platform, lambda v: v.update(version="0.0.1"))
                self.assertProblem("version.json: version")
            with self.subTest(platform=platform, case="unknown key"):
                self.make()
                self.version(platform, lambda v: v.update(mode="0755"))
                self.assertProblem("not the closed version file")
            with self.subTest(platform=platform, case="schema"):
                self.make()
                self.put(f"{d}/{ss.SCHEMA}", b"{}")
                self.assertProblem(f"{ss.SCHEMA}: missing or not the tree's")
            with self.subTest(platform=platform, case="licence"):
                self.make()
                (self.stage / d / "LICENSE.txt").unlink()
                self.assertProblem("LICENSE.txt: missing")
            with self.subTest(platform=platform, case="extra file"):
                self.make()
                self.put(f"{d}/kiln-eml-run", ELF, 0o755)
                self.assertProblem("expected exactly")

    def test_the_linux_helper_is_a_real_elf_executable_and_the_windows_one_a_pe_exe(self):
        self.make()
        d = f"eml/{ss.HELPER_DIR}/linux-x86_64"
        self.put(f"{d}/kiln-eml-inspect", b"#!/bin/sh\necho {}\n", 0o755)
        ss.write_version(str(REPO), str(self.stage / d), "linux-x86_64")
        self.assertProblem("not an x86-64 ELF executable")
        self.make()
        os.chmod(self.stage / d / "kiln-eml-inspect", 0o644)
        self.assertProblem("mode 0o644, expected 0o755")
        self.make()
        os.chmod(self.stage / d / "kiln-eml-inspect", 0o4755)
        self.assertProblem("expected 0o755")
        self.make()
        w = f"eml/{ss.HELPER_DIR}/windows-x86_64"
        self.put(f"{w}/kiln-eml-inspect.exe", pe(True))
        ss.write_version(str(REPO), str(self.stage / w), "windows-x86_64")
        self.assertProblem("not an AMD64 PE32+ executable")

    def test_the_helper_never_outside_component_eml(self):
        self.make()
        self.put(f"core/{ss.HELPER_DIR}/windows-x86_64/kiln-eml-inspect.exe", pe(False))
        self.assertProblem("belongs to component eml")

    def test_a_developer_test_build_is_refused_even_on_the_sfi_branch(self):
        self.make()
        self.put("core/kiln/kiln-core.dll", core_dll() + pc.DEV_MARKER.encode())
        self.assertEqual(self.problems(), [], "the component check itself is about layout")
        found = pc.check_stage(str(self.stage), public=False)
        self.assertTrue(any("a developer test build" in f for f in found), found)

    def test_a_public_cut_stages_the_component_and_the_helper_as_its_guard_says(self):
        self.make()
        found = pc.check_stage(str(self.stage), public=True)
        if pc.STAGE_NAMES or getattr(pc, "STAGE_DIRS", ()):
            # a private build's guard: a public stage refuses the component and the helper
            for rel in ("sfi/kiln/plugins/kiln-sfi.dll", f"eml/{ss.HELPER_DIR}/linux-x86_64/kiln-eml-inspect",
                        f"eml/{ss.HELPER_DIR}/windows-x86_64/kiln-eml-inspect.exe",
                        f"eml/{ss.HELPER_DIR}/windows-x86_64/version.json",
                        f"eml/{ss.HELPER_DIR}/linux-x86_64/LICENSE.txt"):
                self.assertTrue(any(rel in f for f in found), f"{rel} not refused: {found}")
        else:
            # the public guard: they stage like any other component
            self.assertEqual(found, [])
        self.assertEqual(pc.check_stage(str(self.stage), public=False), [])

    def test_the_plugin_list_expects_both(self):
        exp = dict(pl.expected(str(REPO)))
        self.assertEqual(exp.get("kiln/plugins/kiln-sfi.dll"), "sfi")
        self.assertEqual(exp.get("kiln/tools/eml-inspect/linux-x86_64/kiln-eml-inspect"), "eml")
        self.assertEqual(exp.get("kiln/tools/eml-inspect/windows-x86_64/kiln-eml-inspect.exe"), "eml")
        self.make()
        components, files = pl.from_stage(str(self.stage))
        bad = pl.problems([(p, c) for p, c in pl.expected(str(REPO)) if c in ("sfi", "eml")], components, files)
        self.assertEqual([b for b in bad if "kiln-eml.dll" not in b], [])


class StageScript(unittest.TestCase):
    """stage.sh's own lines, run on synthetic build output (no build, no network, nothing executed from the stage)."""
    TEXT = (REPO / "tools/package/stage.sh").read_text()

    def run_part(self, start, end, env_lines, root):
        part = self.TEXT[self.TEXT.index(start):self.TEXT.index(end, self.TEXT.index(start))]
        return subprocess.run(["bash", "-c", "set -euo pipefail\n" + env_lines + "\n" + part, "x", str(REPO), str(root)],
                              capture_output=True, text=True, timeout=60)

    def test_the_plugin_loop_puts_kiln_sfi_in_its_own_component(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            plugins = root / "bin/plugins"
            plugins.mkdir(parents=True)
            for n in ("kiln-sfi.dll", "kiln-emf.dll", "kiln-eml.dll", "kiln-flame.dll", "kiln-shroudforge.dll"):
                (plugins / n).write_bytes(b"MZ " + n.encode())
            r = self.run_part("if [[ -d $bin/plugins ]]; then", "\n# kiln-eml (component eml)",
                              'repo=$1; bin=$2/bin; stage=$2/stage', root)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            staged = sorted(str(p.relative_to(root / "stage")) for p in (root / "stage").rglob("*") if p.is_file())
            self.assertEqual(staged, ["core/kiln/plugins/kiln-flame.dll", "core/kiln/plugins/kiln-shroudforge.dll",
                                      "emf/kiln/plugins/kiln-emf.dll", "eml/kiln/plugins/kiln-eml.dll",
                                      "sfi/kiln/plugins/kiln-sfi.dll"])

    def test_the_helpers_are_staged_with_their_files_and_version(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for platform, exe in ss.PLATFORMS.items():
                p = root / "out/eml-inspect" / platform / exe
                p.parent.mkdir(parents=True)
                p.write_bytes(ELF if platform.startswith("linux") else pe(False))
            # the block's first line, with or without its private marker
            start = next(s for s in ("# sfi-" "private begin: kiln-eml's archive inspector (component eml",
                                     "# kiln-eml's archive inspector (component eml") if s in self.TEXT)
            r = self.run_part(start, "# ShroudForge runtime: a separate GPL-3 component",
                              'repo=$1; out=$2/out; stage=$2/stage; sfi_stage=$repo/tools/sfi/sfi_stage.py; '
                              'die() { echo "$*" >&2; exit 1; }; miss() { echo "MISS $*" >&2; exit 1; }', root)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            stage = root / "stage"
            for platform, exe in ss.PLATFORMS.items():
                d = stage / "eml" / ss.HELPER_DIR / platform
                self.assertEqual(sorted(os.listdir(d)), sorted([exe, "LICENSE.txt", "NOTICE.txt", ss.SCHEMA, "version.json"]))
                mode = stat.S_IMODE(os.stat(d / exe).st_mode)
                self.assertEqual(mode, 0o755 if platform.startswith("linux") else 0o644, platform)
            # with the rest of a stage around them, the check passes
            (stage / "sfi/kiln/plugins").mkdir(parents=True)
            (stage / "sfi/kiln/plugins/kiln-sfi.dll").write_bytes(pe(True, STAMP))
            (stage / "core/kiln/services").mkdir(parents=True)
            (stage / "core/kiln/kiln-core.dll").write_bytes(core_dll())
            (stage / "core/kiln/services/kiln-world.dll").write_bytes(pe(True, STAMP))
            (stage / "shroudforge/kiln/plugins/shroudforge").mkdir(parents=True)
            (stage / "shroudforge/kiln/plugins/shroudforge/shroudforge-runtime.dll").write_bytes(pe(True))
            (stage / "kiln-runtime.json").write_text(json.dumps({"format": 1, "components": COMPONENTS}))
            self.assertEqual(ss.check(str(REPO), str(stage)), [])
            # a missing helper is a missing part
            shutil.rmtree(root / "out/eml-inspect/linux-x86_64")
            shutil.rmtree(stage)
            r = self.run_part(start, "# ShroudForge runtime: a separate GPL-3 component",
                              'repo=$1; out=$2/out; stage=$2/stage; sfi_stage=$repo/tools/sfi/sfi_stage.py; '
                              'die() { echo "$*" >&2; exit 1; }; miss() { echo "MISS $*" >&2; exit 1; }', root)
            self.assertEqual(r.returncode, 1)
            self.assertIn("MISS kiln-eml's archive inspector for linux-x86_64", r.stderr)

    def test_kiln_sfi_is_built_with_the_native_build_stamp(self):
        start = "  # kiln-sfi.dll exports this build's stamp"
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            h = root / "out/cmake/generated/kiln_build.h"
            h.parent.mkdir(parents=True)
            h.write_text(f'#define KILN_BUILD "{META}"\n')
            env = ('repo=$1; out=$2/out; logs=$2/logs; native_build_h=$out/cmake/generated/kiln_build.h; '
                   'built() { printf "%s\\n" "$@" > "$out/built.txt"; }; miss() { echo "MISS $*" >&2; exit 1; }')
            r = self.run_part(start, "  # KILN_EML_INSPECT_LINUX=FILE", env, root)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            args = (root / "out/built.txt").read_text().split("\n")
            self.assertEqual(args[:4], ["kiln-sfi", "env", f"KILN_BUILD={META}", f"CARGO_TARGET_DIR={root}/out/cargo-sfi"])
            self.assertEqual(args[4], f"{REPO}/crates/sfi/kiln-sfi/build.sh")
            # no stamp from the native build: a missing part, never an unstamped kiln-sfi.dll
            h.write_text('#define KILN_BUILD ""\n')
            r = self.run_part(start, "  # KILN_EML_INSPECT_LINUX=FILE", env, root)
            self.assertEqual(r.returncode, 1)
            self.assertIn("MISS plugin kiln-sfi: the native build left no build stamp", r.stderr)

    def test_a_prebuilt_native_build_brings_its_stamp(self):
        part = self.TEXT[self.TEXT.index("  # the native build's stamp: native/cmake/kiln_build_stamp.sh"):
                         self.TEXT.index("  elif ! built native-all")]
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            pre = root / "pre"
            (pre / "plugins").mkdir(parents=True)
            for n in ("kiln-core.dll", "dinput8.dll"):
                (pre / n).write_bytes(pe(True))
            for p in (REPO / "native/plugins").iterdir():
                if (p / "CMakeLists.txt").is_file():
                    (pre / "plugins" / f"kiln-{p.name}.dll").write_bytes(pe(True))
            (pre / "kiln_build.h").write_text(f'#define KILN_BUILD "{META}"\n')
            script = ('set -euo pipefail\nrepo=$1; out=$2/out; KILN_NATIVE_PREBUILT=$2/pre\n'
                      'die() { echo "DIE $*" >&2; exit 1; }; built() { echo "BUILT $1"; }; miss() { echo "MISS $*" >&2; exit 1; }\n'
                      + part + 'fi\necho "native_build_h=$native_build_h"\n')
            run = lambda: subprocess.run(["bash", "-c", script, "x", str(REPO), str(root)], capture_output=True, text=True,
                                         timeout=60)
            r = run()
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn(f"native_build_h={pre}/kiln_build.h", r.stdout)
            self.assertTrue((root / "out/cmake/bin/kiln-core.dll").is_file())
            self.assertFalse((root / "out/cmake/bin/kiln_build.h").exists(), "the stamp is not a staged file")
            (pre / "kiln_build.h").unlink()
            r = run()
            self.assertEqual(r.returncode, 1)
            self.assertIn("holds no kiln_build.h (its build's stamp)", r.stderr)
        # the kiln-win1 build sends its generated kiln_build.h back with the DLLs, and a result without one fails
        remote = (REPO / "tools/release/lib/remote-win1-native.sh").read_text()
        self.assertIn('Copy-Item "\\$J\\out\\cmake\\generated\\kiln_build.h" "\\$b\\kiln_build.h"', remote)
        self.assertIn("\\$keep.Add('kiln_build.h')", remote)
        self.assertIn('grep -q \'^#define KILN_BUILD "+\' "$rwork/bin/kiln_build.h"', remote)

    def test_the_component_table_and_the_checks_are_wired(self):
        self.assertRegex(self.TEXT, r'"sfi": \{"sides": \["server", "client"\], "default": True, "requires": \["core", "eml"\]')
        self.assertRegex(self.TEXT, r'"shroudforge": \{"sides": \["server", "client"\], "default": True')
        self.assertIn('kiln-sfi.*) comp=sfi ;;', self.TEXT)
        self.assertLess(self.TEXT.index("kiln-sfi.*) comp=sfi ;;"), self.TEXT.index("*) comp=core ;;"))
        self.assertIn('"$repo/crates/sfi/kiln-sfi/build.sh"', self.TEXT)
        self.assertIn('"$repo/gpl/kiln-eml/build.sh" --host', self.TEXT)
        check = self.TEXT.index('python3 -I "$sfi_stage" check "$repo" "$stage" || die')
        self.assertGreater(check, self.TEXT.index('tools/release/lib/plugins.py" check'))
        self.assertLess(check, self.TEXT.index('python3 -I "$cut_guard" stage "$repo" "$stage"'))


class SfiBuildScript(unittest.TestCase):
    """crates/sfi/kiln-sfi/build.sh's KILN_BUILD (the stamp build.rs embeds), with a stand-in cargo: nothing is built."""

    def run_build(self, root, env):
        (root / "bin").mkdir(exist_ok=True)
        cargo = root / "bin/cargo"
        cargo.write_text('#!/usr/bin/env bash\nprintf "%s" "${KILN_BUILD-unset}" > "$FAKE_CARGO_OUT"\n')
        cargo.chmod(0o755)
        dll = root / "target/x86_64-pc-windows-msvc/release/kiln_sfi.dll"
        dll.parent.mkdir(parents=True, exist_ok=True)
        dll.write_bytes(pe(True))
        e = {k: v for k, v in os.environ.items() if k not in ("KILN_BUILD", "SOURCE_DATE_EPOCH", "KILN_GIT")}
        e.update(PATH=f"{root}/bin:{os.environ['PATH']}", KILN_OUT=str(root / "out"), CARGO_TARGET_DIR=str(root / "target"),
                 FAKE_CARGO_OUT=str(root / "cargo.txt"), **env)
        (root / "cargo.txt").unlink(missing_ok=True)
        r = subprocess.run(["bash", str(REPO / "crates/sfi/kiln-sfi/build.sh")], env=e, capture_output=True, text=True,
                           timeout=60)
        seen = (root / "cargo.txt").read_text() if (root / "cargo.txt").exists() else None
        return r, seen

    def test_the_stamp_comes_from_the_native_build(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            # given: passed through as it is (tools/package/stage.sh)
            r, seen = self.run_build(root, {"KILN_BUILD": META})
            self.assertEqual((r.returncode, seen), (0, META), r.stderr)
            self.assertIn(f"kiln_build_stamp {VERSION}{META}", r.stdout)
            self.assertTrue((root / "out/cmake/bin/plugins/kiln-sfi.dll").is_file())
            # not given, no native build yet: native/cmake/kiln_build_stamp.sh's rules, into the native build's header
            r, seen = self.run_build(root, {"SOURCE_DATE_EPOCH": "86400", "KILN_GIT": "abc1234"})
            self.assertEqual((r.returncode, seen), (0, "+19700102.abc1234"), r.stderr)
            header = root / "out/cmake/generated/kiln_build.h"
            self.assertEqual(header.read_text(), '#define KILN_BUILD "+19700102.abc1234"\n')
            # not given, the native build's header there: exactly its value (whatever the overrides say now)
            header.write_text('#define KILN_BUILD "+nogit.0123456789abcdef"\n')
            r, seen = self.run_build(root, {"SOURCE_DATE_EPOCH": "86400", "KILN_GIT": "abc1234"})
            self.assertEqual((r.returncode, seen), (0, "+nogit.0123456789abcdef"), r.stderr)
            # set but empty: the version only (untrusted, as a native DLL built without build metadata)
            r, seen = self.run_build(root, {"KILN_BUILD": ""})
            self.assertEqual((r.returncode, seen), (0, ""), r.stderr)
            # a bad value stops the build
            r, seen = self.run_build(root, {"KILN_BUILD": "+bad stamp"})
            self.assertEqual((r.returncode, seen), (2, None))
            self.assertIn("bad KILN_BUILD", r.stderr)

    def test_build_rs_embeds_it(self):
        text = (REPO / "crates/sfi/kiln-sfi/build.rs").read_text()
        self.assertIn('println!("cargo:rerun-if-env-changed=KILN_BUILD");', text)
        self.assertIn('println!("cargo:rustc-env=KILN_SFI_BUILD_STAMP={version}{build}");', text)
        lib = (REPO / "crates/sfi/kiln-sfi/src/lib.rs").read_text()
        self.assertIn('pub extern "C" fn kiln_build_stamp() -> *const core::ffi::c_char {', lib)
        self.assertIn('concat!(env!("KILN_SFI_BUILD_STAMP"), "\\0")', lib)


if __name__ == "__main__":
    unittest.main()
