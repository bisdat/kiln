#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""One release run per work folder and per KILN_GLIBC_DIR, and an export newer than any earlier build
(tools/release/lib/run-locks.sh; docs/kiln/launcher/releasing.md, "One run per work folder and per KILN_GLIBC_DIR").
The release.sh runs here stop at the lock this test holds, before anything is built, removed or written; the scratch
HOME holds an empty stand-in updater key so they get past that check (nothing is signed: the run stops first).
The Cargo case needs cargo and builds a one-file crate in a scratch folder (skipped without cargo).
    python3 -I tools/release/test_release_locks.py"""
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
LIB = HERE / "lib" / "run-locks.sh"
OLD = 1_000_000_000  # 2001: a commit time older than any build


def bash(script, *args, timeout=60, **kw):
    return subprocess.run(["bash", "-c", 'set -euo pipefail; source "$0"; ' + script, str(LIB), *map(str, args)],
                          capture_output=True, text=True, timeout=timeout, **kw)


class Held:
    """flock(2) on FILE from this process, as another release run would hold it."""
    def __init__(self, path, line):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.f = open(path, "a+")
        fcntl.flock(self.f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.f.truncate(0)
        self.f.write(line + "\n")
        self.f.flush()

    def close(self):
        self.f.close()


def free(path):
    with open(path, "a") as f:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except BlockingIOError:
            return False


class Scratch(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="release-locks-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)


class ReleaseLock(Scratch):
    def test_a_second_holder_is_refused_and_named_until_the_first_ends(self):
        d = self.root / "work"
        first = subprocess.Popen(["bash", "-c", 'source "$0"; release_lock "$1" "busy" "release A"; echo taken; read -r _',
                                  str(LIB), str(d)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.addCleanup(lambda: first.poll() is None and first.kill())
        self.assertEqual(first.stdout.readline().strip(), "taken")
        self.assertEqual((d / ".release.lock").read_text(), f"pid {first.pid} release A\n")
        r = bash('release_lock "$1" "another release runs in $1" "release B"', d)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn(f"release: another release runs in {d} (its lock: {d}/.release.lock; held by: pid {first.pid} release A)",
                      r.stderr)
        self.assertNotIn("has ended", r.stderr)
        self.assertEqual((d / ".release.lock").read_text(), f"pid {first.pid} release A\n")  # the refused run wrote nothing
        first.communicate("\n", timeout=30)
        r = bash('release_lock "$1" "busy" "release B" && cat "$1/.release.lock"', d)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stdout, r"^pid \d+ release B\n$")  # the last holder's line replaced, not appended

    def test_a_process_left_behind_keeps_the_lock_and_the_message_says_the_run_ended(self):
        d = self.root / "glibc"
        r = bash('release_lock "$1" "busy" "release A"; sleep 60 >/dev/null 2>&1 & echo "$!"', d)
        self.assertEqual(r.returncode, 0, r.stderr)
        child = int(r.stdout.strip())
        self.addCleanup(lambda: subprocess.run(["kill", str(child)], capture_output=True))
        r = bash('release_lock "$1" "another release builds in KILN_GLIBC_DIR=$1" "release B"', d)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("that run has ended, but a process it started still has the lock open", r.stderr)
        subprocess.run(["kill", str(child)], capture_output=True)
        for _ in range(100):
            if free(d / ".release.lock"):
                break
            time.sleep(0.05)
        self.assertEqual(bash('release_lock "$1" "busy" "release B"', d).returncode, 0)

    def test_a_held_lock_by_another_kind_of_holder_is_refused_too(self):
        d = self.root / "work"
        held = Held(d / ".release.lock", "")  # no line: an older or foreign holder
        self.addCleanup(held.close)
        r = bash('release_lock "$1" "busy" "release B"', d)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("held by: unknown", r.stderr)


class ReleaseShRefuses(Scratch):
    """release.sh itself: a second run stops at the lock, before the --force removal of --out or any write."""
    def setUp(self):
        super().setUp()
        self.home = self.root / "home"
        keys = self.home / ".config" / "kiln-release"
        keys.mkdir(parents=True)
        (keys / "updater.key").write_text("")  # stand-ins: release.sh only checks that they exist before the lock
        (keys / "updater.key.password").write_text("")
        self.version = subprocess.run(["bash", str(HERE / "version.sh"), "dev", "9999"], check=True, capture_output=True,
                                      text=True).stdout.strip()
        self.work, self.glibc, self.out = self.root / "work", self.root / "glibc", self.root / "out"
        self.out.mkdir()
        (self.out / "keep.txt").write_text("a release folder another run still builds\n")

    def release(self, *extra):
        env = {"PATH": os.environ["PATH"], "HOME": str(self.home), "KILN_GLIBC_DIR": str(self.glibc)}
        # --beta 1: a test build, so a missing roadmap entry for the made-up version only warns
        return subprocess.run(["bash", str(HERE / "release.sh"), self.version, "--beta", "1", "--work", str(self.work),
                               "--out", str(self.out), "--force", "--no-nexus", "--skip-windows", *extra],
                              env=env, cwd=self.root, capture_output=True, text=True, timeout=120)

    def assertUntouched(self):
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ["keep.txt"])
        self.assertEqual(sorted(p.name for p in self.work.iterdir()), [".release.lock"])

    def test_a_second_run_on_the_same_work_folder_stops_at_once(self):
        held = Held(self.work / ".release.lock", "pid 4242 release other")
        self.addCleanup(held.close)
        r = self.release()
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn(f"release: another release runs in the work folder {self.work}", r.stderr)
        self.assertIn("held by: pid 4242 release other", r.stderr)
        self.assertUntouched()
        self.assertFalse(self.glibc.exists())

    def test_a_second_run_on_the_same_glibc_dir_stops_at_once(self):
        held = Held(self.glibc / ".release.lock", "pid 4242 release other")
        self.addCleanup(held.close)
        r = self.release()
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn(f"release: another release builds in KILN_GLIBC_DIR={self.glibc}", r.stderr)
        self.assertIn("give this one its own KILN_GLIBC_DIR", r.stderr)
        self.assertUntouched()
        self.assertTrue(free(self.work / ".release.lock"))  # the refused run's work lock ended with it


class FreshMtimes(Scratch):
    def test_every_file_folder_and_link_gets_the_time_and_link_targets_keep_theirs(self):
        src, outside = self.root / "src", self.root / "outside.txt"
        (src / "crates" / "a b").mkdir(parents=True)
        for p in (src / "Cargo.toml", src / "crates" / "a b" / "lib.rs", src / "-dash.rs", outside):
            p.write_text("x")
        (src / "link").symlink_to(outside)
        (src / "dangling").symlink_to(self.root / "nowhere")
        for p in [*src.rglob("*"), src, outside]:
            os.utime(p, (OLD, OLD), follow_symlinks=False)
        now = int(time.time())
        r = bash('fresh_mtimes "$1" "$2"', src, now)
        self.assertEqual(r.returncode, 0, r.stderr)
        for p in [src, *src.rglob("*")]:
            with self.subTest(p=str(p.relative_to(self.root))):
                self.assertEqual(int(p.lstat().st_mtime), now)
        self.assertEqual(int(outside.stat().st_mtime), OLD)

    def test_bad_arguments_are_refused(self):
        for args in ((self.root / "missing", 1), (self.root, "yesterday"), (self.root, "")):
            with self.subTest(args=args):
                self.assertEqual(bash('fresh_mtimes "$1" "$2"', *args).returncode, 2)


class ReleaseShUsesThem(unittest.TestCase):
    """Where release.sh calls them: the locks before the --force removal of --out; the reset after both source tarballs
    and before any build; the Linux CLIs from this run's copies once they are made."""
    text = (HERE / "release.sh").read_text()

    def at(self, needle):
        i = self.text.find(needle)
        self.assertGreaterEqual(i, 0, needle)
        return i

    def test_the_locks_come_before_anything_is_removed(self):
        self.assertLess(self.at('release_lock "$work"'), self.at('release_lock "$glibc"'))
        self.assertLess(self.at('release_lock "$glibc"'), self.at('rm -rf "$out"'))
        self.assertLess(self.at('rm -rf "$out"'), self.at('mkdir -p "$out" "$work"'))

    def test_the_reset_follows_the_source_tarballs_and_precedes_the_builds(self):
        reset = self.at('fresh_mtimes "$src"')
        self.assertLess(self.at('-czf "$work/source/kiln-$version-src.tar.gz" src'), reset)
        self.assertLess(self.at('"$lib/sf-source.sh"'), reset)
        self.assertLess(self.at('KILN_RELEASE_OVERLAY"/. "$src"/'), reset)
        self.assertLess(reset, self.at("# ---------------------------------------------------------------- 2. environment"))

    def test_the_linux_clis_come_from_this_runs_copies(self):
        copied = self.at("kilnctl=$linux_bin/kilnctl")
        # after the copies are made, the target folder is read only to copy kiln-eml-inspect (built later)
        rest = self.text[copied:].replace('install -m 755 "$lin_target/release/kiln-eml-inspect" "$linux_bin/kiln-eml-inspect"', "")
        for name in ("kilnctl", "kiln", "kiln-eml-inspect"):
            with self.subTest(name=name):
                self.assertEqual(rest.count(f'"$lin_target/release/{name}"'), 0)
        self.assertIn('install -m 755 "$linux_bin/kilnctl" "$out/runtime/kilnctl"', self.text)
        self.assertIn('install -m 755 "$linux_bin/kiln" "$out/sdk/kiln"', self.text)
        self.assertIn("export KILN_EML_INSPECT_LINUX=$linux_bin/kiln-eml-inspect", self.text)


@unittest.skipUnless(shutil.which("cargo"), "needs cargo")
class CargoKeepsAnOlderDatedChange(Scratch):
    """The bug the reset fixes: an export of an older commit (files dated to it) into a target folder that built a
    later one keeps the later code; after fresh_mtimes, Cargo rebuilds it."""
    def test(self):
        crate = self.root / "demo"
        (crate / "src").mkdir(parents=True)
        (crate / "Cargo.toml").write_text('[package]\nname = "demo"\nversion = "0.1.0"\nedition = "2021"\n')
        main = crate / "src" / "main.rs"
        env = {**os.environ, "CARGO_TARGET_DIR": str(self.root / "target"), "CARGO_BUILD_BUILD_DIR": str(self.root / "build")}
        env.pop("RUSTFLAGS", None)
        env.pop("CARGO_ENCODED_RUSTFLAGS", None)

        def build_and_run():
            subprocess.run(["cargo", "build", "--offline", "-q"], cwd=crate, env=env, check=True, capture_output=True, timeout=300)
            return subprocess.run([str(self.root / "target" / "debug" / "demo")], capture_output=True, text=True,
                                  check=True).stdout.strip()

        main.write_text('fn main() { println!("later commit"); }\n')
        self.assertEqual(build_and_run(), "later commit")
        main.write_text('fn main() { println!("older commit"); }\n')
        for p in (main, crate / "src", crate / "Cargo.toml", crate):
            os.utime(p, (OLD, OLD))  # what git archive does for a commit of 2001
        self.assertEqual(build_and_run(), "later commit", "Cargo rebuilt an older-dated change by itself: the reset is not needed")
        time.sleep(1.1)  # the reset's time (whole seconds) after the last build, as in a release run
        r = bash('fresh_mtimes "$1" "$(date +%s)"', crate)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(build_and_run(), "older commit")


if __name__ == "__main__":
    unittest.main()
