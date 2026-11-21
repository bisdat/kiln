#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for public_cut.py, the stage guard of Kiln's public and private builds, and for its two hooks in
tools/package/stage.sh: the files and words of a private build stop a public stage, a developer test build stops every
stage, and kiln-sfi.dll and kiln-eml's archive inspector stage like any other component. Synthetic repositories and
stages in a temporary folder; no build.
    python3 -I tools/sfi/test_public_cut.py"""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
GUARD = HERE / "public_cut.py"
spec = importlib.util.spec_from_file_location("public_cut", GUARD)
pc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pc)


def example(pattern):
    """A path the glob matches: ** and * become one plain folder or name."""
    return pattern.replace("/**", "/x/y.txt").replace("**", "x").replace("*", "x")


class Repo:
    """A synthetic git repository with the guard in it."""

    def __init__(self, root, branch="main"):
        self.root = root
        self.home = root.parent / (root.name + "-home")
        self.home.mkdir(exist_ok=True)
        (root / "tools/sfi").mkdir(parents=True)
        shutil.copy(GUARD, root / "tools/sfi/public_cut.py")
        self.write("README.md", "Kiln\n")
        self.git("init", "-q", "-b", branch)
        self.commit()

    def env(self, **extra):
        env = {"PATH": os.environ["PATH"], "HOME": str(self.home), "GIT_CONFIG_NOSYSTEM": "1", "LANG": "C.UTF-8"}
        env.update(extra)
        return env

    def git(self, *args):
        return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c",
                               "commit.gpgsign=false", *args], cwd=self.root, env=self.env(), check=True,
                              capture_output=True, text=True)

    def commit(self):
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", "x")

    def write(self, rel, text):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(text) if isinstance(text, bytes) else p.write_text(text)
        return p

    def guard(self, *args, **env):
        return subprocess.run(["python3", "-I", str(self.root / "tools/sfi/public_cut.py"), *map(str, args)],
                              env=self.env(**env), capture_output=True, text=True, timeout=120)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="public-cut-")
        self.addCleanup(self.tmp.cleanup)
        self.top = Path(self.tmp.name)

    def repo(self, name="r", **kw):
        return Repo(self.top / name, **kw)


class Tables(unittest.TestCase):
    def test_the_private_set_is_small_and_names_no_public_component(self):
        self.assertTrue(pc.PRIVATE_PATHS)
        self.assertIn("sfi-private", pc.PRIVATE_TOKENS)
        self.assertIn("KILN_SFI_DEV_" + "SF18_TEST", pc.PRIVATE_TOKENS)
        for public in ("crates/sfi/kiln-sfi/src/lib.rs", "crates/sfi/sfi-core/src/lib.rs", "gpl/kiln-eml/kiln-eml/src/startup.rs",
                       "gpl/kiln-eml/kiln-eml/src/kiln_eml_inspect.rs", "native/core/gates/sfi.json",
                       "tools/sfi/public_cut.py", "tools/sfi/sfi_stage.py", "schema/sfi/sfi-state.schema.json"):
            self.assertFalse(pc.private_path(public), public)
        # what a public stage holds is no private build output any more
        self.assertEqual(pc.STAGE_NAMES, ())
        self.assertEqual(pc.STAGE_TOKENS, ())
        self.assertEqual(getattr(pc, "STAGE_DIRS", ()), ())


class Source(Base):
    def test_a_clean_public_tree_passes(self):
        r = self.repo()
        r.write("native/core/src/gates.cpp", "// the client gate\n")
        r.write("crates/sfi/kiln-sfi/src/lib.rs", "// kiln-sfi\n")
        r.commit()
        p = r.guard("source", r.root)
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_every_private_path_stops_a_public_cut(self):
        for i, pattern in enumerate(pc.PRIVATE_PATHS):
            rel = example(pattern)
            with self.subTest(rel=rel):
                self.assertTrue(pc.private_path(rel), rel)
                r = self.repo(name=f"path{i}")
                r.write(rel, "x\n")
                r.commit()
                p = r.guard("source", r.root)
                self.assertEqual(p.returncode, 1, p.stderr)
                self.assertIn(f"private file: {rel}", p.stderr)
                self.assertIn("REFUSED", p.stderr)

    def test_every_private_word_in_a_shared_file_stops_a_public_cut(self):
        for i, token in enumerate(pc.PRIVATE_TOKENS):
            with self.subTest(token=token):
                r = self.repo(name=f"token{i}")
                r.write("native/core/src/y.cpp", f"// {token}\n")
                r.commit()
                p = r.guard("source", r.root)
                self.assertEqual(p.returncode, 1, p.stderr)
                self.assertIn("private change in native/core/src/y.cpp", p.stderr)

    def test_a_case_change_or_a_link_does_not_hide_a_private_path(self):
        rel = example(pc.PRIVATE_PATHS[0])
        r = self.repo()
        r.write(rel.upper(), "x\n")
        r.commit()
        self.assertEqual(r.guard("source", r.root).returncode, 1)
        r = self.repo(name="link")
        r.write(rel, "x\n")
        os.symlink(rel, r.root / "innocent.txt")
        r.commit()
        p = r.guard("source", r.root)
        self.assertIn("innocent.txt", p.stderr)

    def test_an_sfi_branch_is_a_private_stage_but_never_a_release_build(self):
        r = self.repo(branch="sfi/x")
        r.write(example(pc.PRIVATE_PATHS[0]), "x\n")
        r.commit()
        p = r.guard("source", r.root)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("private stage (branch sfi/x)", p.stderr)
        p = r.guard("source", r.root, KILN_RELEASE_BUILD="1")
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn("a release build", p.stderr)

    def test_usage(self):
        r = self.repo()
        self.assertEqual(r.guard().returncode, 2)
        self.assertEqual(r.guard("source").returncode, 2)
        self.assertEqual(r.guard("source", r.root / "nothing").returncode, 2)


class Stage(Base):
    def stage(self, files):
        d = self.top / "stage"
        shutil.rmtree(d, ignore_errors=True)
        for rel, data in files.items():
            p = d / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        return d

    def test_a_developer_test_build_is_refused_on_every_branch(self):
        for branch in ("main", "sfi/x"):
            with self.subTest(branch=branch):
                r = self.repo(name=branch.replace("/", "_"), branch=branch)
                d = self.stage({"core/kiln/kiln-core.dll": b"MZ" + pc.DEV_MARKER.encode() + b"\0"})
                p = r.guard("stage", r.root, d)
                self.assertEqual(p.returncode, 1, p.stderr)
                self.assertIn("a developer test build", p.stderr)
                d = self.stage({"core/kiln/kiln-core.dll": b"MZ" + pc.DEV_MARKER.encode("utf-16-le")})
                self.assertEqual(r.guard("stage", r.root, d).returncode, 1)

    def test_the_interpreter_and_the_archive_inspector_stage_like_any_component(self):
        r = self.repo()
        d = self.stage({"core/kiln/kiln-core.dll": b"MZ kiln-sfi.interpreter kiln-sfi.dll\0",
                        "sfi/kiln/plugins/kiln-sfi.dll": b"MZ kiln.internal.launch\0",
                        "eml/kiln/plugins/kiln-eml.dll": b"MZ kiln.eml.archive kiln_eml_startup\0",
                        "eml/kiln/tools/eml-inspect/linux-x86_64/kiln-eml-inspect": b"\x7fELF",
                        "eml/kiln/tools/eml-inspect/linux-x86_64/version.json": b'{"name": "kiln-eml-inspect"}',
                        "eml/kiln/tools/eml-inspect/windows-x86_64/kiln-eml-inspect.exe": b"MZ"})
        p = r.guard("stage", r.root, d)
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_a_link_in_the_stage_is_refused(self):
        r = self.repo()
        d = self.stage({"core/kiln/a.txt": b"a"})
        os.symlink(d / "core/kiln/a.txt", d / "core/kiln/b.txt")
        p = r.guard("stage", r.root, d)
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn("a link in the stage", p.stderr)


class StageScript(unittest.TestCase):
    def test_stage_sh_runs_the_guard_before_the_build_and_on_the_stage(self):
        s = (REPO / "tools/package/stage.sh").read_text()
        self.assertIn('python3 -I "$cut_guard" source "$repo"', s)
        self.assertIn('python3 -I "$cut_guard" stage "$repo" "$stage"', s)
        self.assertLess(s.index('"$cut_guard" source'), s.index("collect kiln-eml"))
        self.assertGreater(s.index('"$cut_guard" stage'), s.index("plugins.py\" check"))


class ThisTree(unittest.TestCase):
    def test_this_tree_holds_no_private_path_or_word(self):
        if not (REPO / ".git").exists():
            self.skipTest("not a git checkout")
        self.assertEqual(pc.check_source(str(REPO)), [])


if __name__ == "__main__":
    unittest.main()
