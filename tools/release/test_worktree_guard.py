#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""release.sh --worktree only in a plain test run (an explicit --out that is not a release folder, ~/Kiln-release/kiln-*),
and no publish step takes a test build of a working tree. Argument handling only: every release.sh run here stops
before its first build, at the updater key check of a scratch HOME (no key there), so nothing is built, signed or read
from the real ~/.config/kiln-release.   python3 -I tools/release/test_worktree_guard.py"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("kiln_nexus", HERE / "nexus.py")
nexus = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nexus)

REFUSED = "--worktree is for test runs only"
NO_KEY = "no updater key at"  # the check after the guard: the run got past it (and stops there, before any work)


class WorktreeGuard(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="release-worktree-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.version = subprocess.run(["bash", str(HERE / "version.sh"), "dev", "9999"], check=True, capture_output=True,
                                      text=True).stdout.strip()
        self.work = self.root / "work"

    def release(self, *args):
        env = {"PATH": os.environ["PATH"], "HOME": str(self.home), "KILN_GLIBC_DIR": str(self.root / "glibc")}
        return subprocess.run(["bash", str(HERE / "release.sh"), self.version, "--work", str(self.work), *map(str, args)],
                              env=env, cwd=self.root, capture_output=True, text=True, timeout=60)

    def assertNothingMade(self):
        self.assertFalse(self.work.exists())
        self.assertEqual(sorted(p.name for p in self.home.iterdir()), [])

    def test_worktree_without_out_is_refused_before_anything(self):
        r = self.release("--worktree")
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn(REFUSED, r.stderr)
        self.assertIn("needs --out DIR, not a release folder", r.stderr)
        self.assertNotIn(NO_KEY, r.stderr)
        self.assertNothingMade()

    def test_worktree_into_the_release_folder_is_refused(self):
        releases = self.home / "Kiln-release"
        (self.root / "link").symlink_to(releases, target_is_directory=True)
        for out in (releases / f"kiln-{self.version}", releases, releases / "kiln-x" / "sub", releases / "kiln-x.github", releases / "kiln-x.nexus",
                    str(releases / "kiln-x") + "/", releases / "trial" / ".." / "kiln-x",
                    self.root / "link" / "kiln-x", Path("home/Kiln-release/kiln-rel")):  # the last: relative to the cwd
            with self.subTest(out=str(out)):
                r = self.release("--worktree", "--out", out)
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertIn(REFUSED, r.stderr)
                self.assertIn("is a release folder", r.stderr)
                self.assertNothingMade()

    def test_release_folder_symlink_to_elsewhere_is_still_refused(self):
        releases = self.home / "Kiln-release"
        releases.mkdir()
        target = self.root / "existing-release"
        target.mkdir()
        link = releases / "kiln-existing"
        link.symlink_to(target, target_is_directory=True)
        for out in (link, link / "sub"):
            with self.subTest(out=str(out)):
                r = self.release("--worktree", "--out", out, "--force")
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertIn(REFUSED, r.stderr)
                self.assertNotIn(NO_KEY, r.stderr)
                self.assertFalse(self.work.exists())
                self.assertEqual(list(target.iterdir()), [])

    def test_the_order_of_the_flags_does_not_matter(self):
        r = subprocess.run(["bash", str(HERE / "release.sh"), "--worktree", "--out", str(self.home / "Kiln-release" / f"kiln-{self.version}"),
                            self.version], env={"PATH": os.environ["PATH"], "HOME": str(self.home)}, cwd=self.root,
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn(REFUSED, r.stderr)

    def test_parser_variants_cannot_bypass_the_guard(self):
        release = self.home / "Kiln-release" / "kiln-x"
        for args in (("--worktree", "--worktree", "--out", release),
                     ("--out", self.root / "trial", "--out", release, "--worktree")):
            with self.subTest(args=args):
                r = self.release(*args)
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertIn(REFUSED, r.stderr)
                self.assertNothingMade()
        # Neither --out=DIR nor -- is supported by this parser; both fail closed.
        for args in (("--worktree", f"--out={release}"), ("--", "--worktree")):
            with self.subTest(args=args):
                r = self.release(*args)
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertIn("unknown option", r.stderr)
                self.assertNothingMade()

    def test_a_trial_with_its_own_out_still_runs(self):
        # dev.28's trial went to ~/Kiln-release/trial/kiln-0.2.0-dev.28: a test folder, not a release folder
        for out in (self.home / "Kiln-release" / "trial" / f"kiln-{self.version}", self.root / "trial",
                    self.home / "Kiln-release-trial" / "kiln-x", self.home / "Kiln-releases"):
            with self.subTest(out=str(out)):
                r = self.release("--worktree", "--out", out, "--no-nexus", "--skip-windows")
                self.assertNotIn(REFUSED, r.stderr)
                self.assertIn(NO_KEY, r.stderr)  # past the guard; the scratch HOME has no updater key
                self.assertEqual(r.returncode, 1, r.stderr)
                self.assertFalse(out.exists())

    def test_a_release_of_a_commit_is_not_affected(self):
        for args in ((), ("--out", self.home / "Kiln-release" / f"kiln-{self.version}")):
            with self.subTest(args=args):
                r = self.release(*args)
                self.assertNotIn(REFUSED, r.stderr)
                self.assertIn(NO_KEY, r.stderr)
                self.assertEqual(r.returncode, 1, r.stderr)

    def test_the_closing_text_of_a_test_build_names_no_publish_step(self):
        script = (HERE / "release.sh").read_text(encoding="utf-8")
        block = script.split("# the closing text:", 1)[1].split("\n", 1)[1].split('\nstep "done"', 1)[0]
        for worktree in (1, 0):
            r = subprocess.run(["bash", "-euo", "pipefail", "-c", block + '\nprintf "%s" "$closing"'],
                               env={"PATH": os.environ["PATH"], "worktree": str(worktree), "version": "0.2.0-dev.9999",
                                    "out": "/x/out", "log": "/x/log"}, capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stderr)
            if worktree:
                self.assertIn("Test build 0.2.0-dev.9999 of the working tree", r.stdout)
                self.assertIn("never published", r.stdout)
                self.assertNotIn("Next (by hand", r.stdout)
            else:
                self.assertIn("tools/release/publish.sh /x/out", r.stdout)
        # a test build (release.sh --beta N): its own next steps, the beta channel only, never Nexus Mods
        r = subprocess.run(["bash", "-euo", "pipefail", "-c", block + '\nprintf "%s" "$closing"'],
                           env={"PATH": os.environ["PATH"], "worktree": "0", "version": "0.2.0-dev.29", "name": "0.2.0-dev.29-beta.3",
                                "channel": "beta", "beta": "3", "out": "/x/out", "log": "/x/log"}, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Test build 3 of 0.2.0-dev.29 (beta channel)", r.stdout)
        self.assertIn("v0.2.0-dev.29-beta.3", r.stdout)
        self.assertNotIn("nexus.py", r.stdout)

    def test_usage_text_says_so(self):
        r = subprocess.run(["bash", str(HERE / "release.sh"), "--help"], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0)
        self.assertIn("explicit --out that is not a release folder (~/Kiln-release/kiln-*", " ".join(r.stdout.split()))


class PublishRefusesATestBuild(unittest.TestCase):
    """github-draft.sh refused a folder not built from a commit already; publish.sh and nexus.py --publish now do too."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="release-testbuild-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def folder(self, source):
        d = self.root / ("rel-" + str(abs(hash(source))))
        (d / "updates" / "github").mkdir(parents=True)
        (d / "updates" / "github" / "latest.json").write_text('{"version": "0.2.0-dev.9999"}', encoding="utf-8")
        (d / "README.txt").write_text(f"Kiln 0.2.0-dev.9999\nBuilt 2026-11-14 from {source} (dev channel).\n", encoding="utf-8")
        return d

    def publish(self, d):
        bin_ = self.root / "bin"
        bin_.mkdir(exist_ok=True)
        marker = self.root / "remote-called"
        for tool in ("ssh", "rsync", "gh", "curl", "scp"):  # any remote step would leave the marker
            p = bin_ / tool
            p.write_text(f'#!/bin/sh\necho {tool} >> "{marker}"\nexit 1\n', encoding="utf-8")
            p.chmod(0o755)
        env = {"PATH": f"{bin_}:{os.environ['PATH']}", "HOME": str(self.root / "home")}
        r = subprocess.run(["bash", str(HERE / "publish.sh"), str(d), "--host", "nowhere.invalid", "--yes"], env=env,
                           capture_output=True, text=True, timeout=60)
        return r, marker

    def test_publish_sh_refuses_a_test_build_before_anything(self):
        d = self.folder("the working tree at 0123abc (a test build, not a release)")
        r, marker = self.publish(d)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("was not built from a commit", r.stderr)
        self.assertNotIn("== checking", r.stdout)
        self.assertFalse(marker.exists())
        (d / "README.txt").unlink()  # no README at all: refused the same way
        r, _ = self.publish(d)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("was not built from a commit", r.stderr)

    def test_publish_sh_goes_on_for_a_commit(self):
        r, marker = self.publish(self.folder("commit 0123abc"))
        self.assertNotIn("was not built from a commit", r.stderr)
        self.assertIn("== checking", r.stdout)  # then the SHA256SUMS check fails on this fake folder, before any upload
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse(marker.exists())

    def nexus(self, d, *args):
        out, err = io.StringIO(), io.StringIO()
        argv = [str(d), *args, "--api", "http://127.0.0.1:9/v3", "--env", str(self.root / "absent.env"),
                "--state", str(self.root / "state.json")]
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = nexus.run(argv)
        return code, err.getvalue()

    def test_nexus_publish_refuses_a_test_build_before_reading_the_key(self):
        test_build = self.folder("the working tree at 0123abc (a test build, not a release)")
        code, err = self.nexus(test_build, "--publish")
        self.assertEqual(code, 2)
        self.assertIn("was not built from a commit", err)
        # a dry run (GET only) and a folder built from a commit go on to the key (absent here)
        for d, args in ((test_build, ()), (self.folder("commit 0123abc"), ("--publish",))):
            code, err = self.nexus(d, *args)
            self.assertEqual(code, 2)
            self.assertNotIn("was not built from a commit", err)
            self.assertIn("no Nexus API key", err)

    def test_built_from_commit_reads_readme_like_github_draft(self):
        self.assertEqual(nexus.built_from_commit(str(self.folder("commit 0123abc"))), "0123abc")
        self.assertIsNone(nexus.built_from_commit(str(self.folder("the working tree at 0123abc (a test build, not a release)"))))
        self.assertIsNone(nexus.built_from_commit(str(self.root / "missing")))


if __name__ == "__main__":
    unittest.main()
