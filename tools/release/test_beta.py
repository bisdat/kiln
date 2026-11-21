#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The release tooling for test builds (the beta channel, 0.2.0-dev.29; docs/kiln/launcher/releasing.md "Cutting a
beta"), offline: release.py's beta manifest and build order (the launcher's selfupdate/feed.rs Build order), and dry
runs of github-draft.sh and publish.sh on made-up release folders, signed with a throwaway minisign key made here.
A test build moves only the beta channel (frostdev.io updates/beta, the updates-beta pointer), never dev, stable,
updates-dev, "latest" or Nexus Mods, and publishes no SDK; a release moves the beta channel too.

    python3 -I tools/release/test_beta.py
"""
import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "lib"))
import release  # noqa: E402

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey  # noqa: E402
from cryptography.hazmat.primitives import serialization  # noqa: E402


class Key:
    """A throwaway minisign key in Tauri's spelling (its pubkey and .sig files are base64 of minisign's text)."""

    def __init__(self):
        self.sk = Ed25519PrivateKey.generate()
        self.id = os.urandom(8)
        pk = self.sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        text = "untrusted comment: minisign public key\n" + base64.b64encode(b"Ed" + self.id + pk).decode() + "\n"
        self.pub = base64.b64encode(text.encode()).decode()

    def sign(self, path, version):
        data = Path(path).read_bytes()
        sig = self.sk.sign(hashlib.blake2b(data, digest_size=64).digest())
        trusted = f"timestamp:1794700000\tfile:{Path(path).name}\tversion:{version}"
        glob = self.sk.sign(sig + trusted.encode())
        text = ("untrusted comment: signature from tauri secret key\n" + base64.b64encode(b"ED" + self.id + sig).decode() + "\n"
                + "trusted comment: " + trusted + "\n" + base64.b64encode(glob).decode() + "\n")
        Path(str(path) + ".sig").write_text(base64.b64encode(text.encode()).decode())


def run(*cmd, cwd=None, env=None):
    return subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, capture_output=True, text=True)


class ReleasePy(unittest.TestCase):
    def test_build_order_is_the_launchers(self):
        line = ["0.2.0-dev.28", "0.2.0-dev.29+beta.1", "0.2.0-dev.29+beta.2", "0.2.0-dev.29+beta.10", "0.2.0-dev.29", "0.2.0-dev.30+beta.1", "0.2.0"]
        for i, a in enumerate(line):
            for j, b in enumerate(line):
                self.assertEqual(release.build_order(a, b), (i > j) - (i < j), (a, b))
        # other build metadata is no beta number and decides nothing
        self.assertEqual(release.build_order("0.2.0-dev.29+20261115.abc", "0.2.0-dev.29"), 0)
        self.assertIsNone(release.build_beta("0.2.0-dev.29+beta.0"))
        self.assertEqual(release.build_beta("0.2.0-dev.29+beta.3"), 3)
        out = run(sys.executable, "-I", HERE / "lib/release.py", "order", "0.2.0-dev.29+beta.2", "0.2.0-dev.29")
        self.assertEqual(out.stdout.strip(), "-1", out.stderr)

    def test_a_beta_manifest_names_its_build_and_number(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d, "kiln-launcher_0.2.0-dev.29-beta.3_amd64.AppImage")
            f.write_bytes(b"\x7fELF test")
            Key().sign(f, "0.2.0-dev.29+beta.3")
            out = Path(d, "beta/latest.json")
            entry = f"linux-x86_64-appimage,linux-x86_64=https://frostdev.io/kiln/downloads/0.2.0-dev.29-beta.3/linux/{f.name}|{f}.sig|{f}"
            r = run(sys.executable, "-I", HERE / "lib/release.py", "manifest", "--version", "0.2.0-dev.29+beta.3", "--channel", "beta",
                    "--beta", "3", "--entry", entry, "--out", out)
            self.assertEqual(r.returncode, 0, r.stderr)
            m = json.loads(out.read_text())
            self.assertEqual(m["version"], "0.2.0-dev.29+beta.3")
            self.assertEqual((m["kiln"]["channel"], m["kiln"]["beta"]), ("beta", 3))
            # a beta number that does not fit the version, or the wrong channel, is refused
            for args in (["--version", "0.2.0-dev.29+beta.3", "--channel", "beta", "--beta", "4"],
                         ["--version", "0.2.0-dev.29+beta.3", "--channel", "dev"],
                         ["--version", "0.2.0-dev.29", "--channel", "beta", "--beta", "3"]):
                r = run(sys.executable, "-I", HERE / "lib/release.py", "manifest", *args, "--entry", entry, "--out", out)
                self.assertNotEqual(r.returncode, 0, args)
            # a release may serve the beta channel (testers are offered the final build)
            r = run(sys.executable, "-I", HERE / "lib/release.py", "manifest", "--version", "0.2.0-dev.29", "--channel", "beta",
                    "--entry", entry, "--out", out)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertNotIn("beta", json.loads(out.read_text())["kiln"])


class ReleaseArgs(unittest.TestCase):
    """release.sh's --beta and --channel, argument handling only: every run stops at the updater key check of a scratch
    HOME (no key there), before anything is built or read from the real ~/.config/kiln-release."""

    NO_KEY = "no updater key at"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="release-beta-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "home").mkdir()
        self.version = run("bash", HERE / "version.sh", "dev", "9999").stdout.strip()

    def release(self, *args):
        env = {"PATH": os.environ["PATH"], "HOME": str(self.root / "home"), "KILN_GLIBC_DIR": str(self.root / "glibc")}
        return run("bash", HERE / "release.sh", self.version, "--work", self.root / "work", *args, cwd=self.root, env=env)

    def test_a_test_build_is_asked_for_by_its_number(self):
        for args in (["--beta", "3"], ["--channel", "beta", "--beta", "3"], ["--channel", "dev"], []):
            r = self.release(*args)
            self.assertIn(self.NO_KEY, r.stderr, args)
            self.assertEqual(r.returncode, 1, (args, r.stderr))
        for args, why in ((["--beta", "0"], "test build's number"), (["--beta", "x"], "test build's number"), (["--beta", "03"], "test build's number"),
                          (["--channel", "beta"], "needs --beta N"), (["--channel", "stable"], "does not fit"),
                          (["--channel", "dev", "--beta", "3"], "for the beta channel"), (["--channel", "nightly"], "stable, dev or beta")):
            r = self.release(*args)
            self.assertEqual(r.returncode, 2, (args, r.stderr))
            self.assertIn(why, r.stderr, args)
            self.assertNotIn(self.NO_KEY, r.stderr)
        self.assertFalse((self.root / "work").exists())


class Scripts(unittest.TestCase):
    """github-draft.sh and publish.sh, dry runs, in a copy of tools/release inside a throwaway git repository."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="kiln-beta-"))
        self.repo = self.tmp / "repo"
        shutil.copytree(HERE, self.repo / "tools/release", ignore=shutil.ignore_patterns("__pycache__", "linux-build"))
        self.key = Key()
        conf = self.repo / "launcher/src-tauri/tauri.conf.json"
        conf.parent.mkdir(parents=True)
        conf.write_text(json.dumps({"plugins": {"updater": {"pubkey": self.key.pub}}}))
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid", GIT_COMMITTER_NAME="t",
                   GIT_COMMITTER_EMAIL="t@example.invalid", GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")
        self.env = env
        for c in (["git", "init", "-q"], ["git", "add", "-A"], ["git", "commit", "-q", "-m", "fixture"]):
            r = run(*c, cwd=self.repo, env=env)
            self.assertEqual(r.returncode, 0, r.stderr)
        self.commit = run("git", "rev-parse", "--short=7", "HEAD", cwd=self.repo).stdout.strip()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def folder(self, version, beta=None):
        """A release folder as release.sh makes it (the parts the two scripts read)."""
        name = f"{version}-beta.{beta}" if beta else version
        bid = f"{version}+beta.{beta}" if beta else version
        d = self.tmp / f"kiln-{name}"
        for sub in ("linux", "windows", "runtime", "sdk", "samples", "source"):
            (d / sub).mkdir(parents=True, exist_ok=True)
        (d / "README.txt").write_text(f"Kiln {name}\nBuilt 2026-11-15 from commit {self.commit} ({'beta' if beta else 'dev'} channel).\n")
        (d / "RELEASE-NOTES.md").write_text(f"# Kiln {name}\n")
        (d / "THIRD-PARTY-NOTICES.txt").write_text("notices\n")
        app = d / "linux" / f"kiln-launcher_{name}_amd64.AppImage"
        app.write_bytes(b"\x7fELF " + name.encode())
        self.key.sign(app, bid)
        (d / "linux" / f"kiln-launcher_{name}_amd64.deb").write_bytes(b"deb")
        setup = d / "windows" / f"kiln-launcher_{name}_x64-setup.exe"
        setup.write_bytes(b"MZ " + name.encode())
        self.key.sign(setup, bid)
        for f in (f"runtime/kiln-runtime-{version}.zip", f"runtime/kiln-server-{version}.zip", "runtime/INSTALL-RUNTIME.txt", "runtime/kilnctl",
                  f"sdk/Kiln.Sdk.{version}.nupkg", "sdk/kiln", "sdk/kiln-vscode-0.1.0.vsix", "samples/hello-1.0.0.zip", f"source/kiln-{version}-src.tar.gz"):
            (d / f).write_bytes(b"x")
        channels = ["beta"] if beta else ["dev", "beta"]
        for ch in channels + ["github"]:
            base = f"https://github.com/frostdev-ops/kiln/releases/download/v{name}" if ch == "github" else f"https://frostdev.io/kiln/downloads/{name}"
            out = d / "updates" / ("github" if ch == "github" else f"frostdev.io/{ch}") / "latest.json"
            mch = "beta" if beta else ("beta" if ch == "beta" else "dev")
            args = ["--beta", str(beta)] if beta else []
            r = run(sys.executable, "-I", HERE / "lib/release.py", "manifest", "--version", bid, "--channel", mch, *args,
                    "--entry", f"linux-x86_64-appimage,linux-x86_64={base}/linux/{app.name}|{app}.sig|{app}", "--out", out)
            self.assertEqual(r.returncode, 0, r.stderr)
        r = run(sys.executable, "-I", HERE / "lib/release.py", "sums", d)
        self.assertEqual(r.returncode, 0, r.stderr)
        return d

    def script(self, name, *args):
        return run("bash", self.repo / "tools/release" / name, *args, cwd=self.repo, env=self.env)

    def test_a_test_build_is_a_draft_pre_release_without_the_sdk(self):
        d = self.folder("0.2.0-dev.29", beta=3)
        r = self.script("github-draft.sh", d, "--dry-run")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("v0.2.0-dev.29-beta.3", r.stdout)
        self.assertIn("--prerelease", r.stdout)
        self.assertIn("Kiln\\ 0.2.0-dev.29\\ beta\\ 3\\ \\(test\\ build\\)", r.stdout)
        staged = sorted(p.name for p in Path(str(d) + ".github").iterdir())
        self.assertIn("kiln-launcher_0.2.0-dev.29-beta.3_amd64.AppImage", staged)
        self.assertIn("latest.json", staged)
        for gone in ("Kiln.Sdk.0.2.0-dev.29.nupkg", "kiln-vscode-0.1.0.vsix", "kiln-0.2.0-dev.29-linux-x86_64", "kiln-samples-0.2.0-dev.29.zip"):
            self.assertNotIn(gone, staged)
        # a release keeps its SDK and samples
        d = self.folder("0.2.0-dev.29")
        r = self.script("github-draft.sh", d, "--dry-run")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("v0.2.0-dev.29 ", r.stdout)
        self.assertIn("Kiln.Sdk.0.2.0-dev.29.nupkg", sorted(p.name for p in Path(str(d) + ".github").iterdir()))

    def test_a_test_build_moves_only_the_beta_channel(self):
        d = self.folder("0.2.0-dev.29", beta=3)
        r = self.script("publish.sh", d, "--host", "example-host", "--dry-run")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        out = r.stdout
        self.assertIn("channels to move: beta", out)
        self.assertIn("downloads/0.2.0-dev.29-beta.3/", out)
        self.assertIn("updates/beta/latest.json", out)
        self.assertIn("gh release edit v0.2.0-dev.29-beta.3 --repo frostdev-ops/kiln --draft=false --prerelease", out)
        self.assertIn("gh release upload updates-beta", out)
        self.assertIn("--exclude /sdk/", out)
        for never in ("updates/dev", "updates/stable", "updates-dev", "--latest", "nexus.py", "roadmap.py"):
            self.assertNotIn(never, out, never)
        # never Nexus Mods, never a folder that holds another channel's manifest
        r = self.script("publish.sh", d, "--host", "example-host", "--dry-run", "--nexus")
        self.assertNotEqual(r.returncode, 0)
        extra = d / "updates/frostdev.io/dev/latest.json"
        extra.parent.mkdir(parents=True)
        shutil.copy(d / "updates/frostdev.io/beta/latest.json", extra)
        run(sys.executable, "-I", HERE / "lib/release.py", "sums", d)
        r = self.script("publish.sh", d, "--host", "example-host", "--dry-run")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("must not hold", r.stderr)

    def test_a_release_moves_dev_and_beta_unless_told(self):
        d = self.folder("0.2.0-dev.29")
        r = self.script("publish.sh", d, "--host", "example-host", "--dry-run")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("channels to move: beta dev", r.stdout)
        self.assertIn("gh release upload updates-dev", r.stdout)
        self.assertIn("gh release upload updates-beta", r.stdout)
        self.assertNotIn("--exclude /sdk/ ", r.stdout)
        # the roadmap after GitHub (tools/release/roadmap.py): shown, not run
        self.assertIn("== Roadmap", r.stdout)
        self.assertIn("roadmap.py ship 0.2.0-dev.29", r.stdout)
        self.assertIn("roadmap.py deploy", r.stdout)
        self.assertLess(r.stdout.index("gh release edit v0.2.0-dev.29"), r.stdout.index("roadmap.py ship"))
        r = self.script("publish.sh", d, "--host", "example-host", "--dry-run", "--no-beta-channel")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("channels to move: dev", r.stdout)
        self.assertNotIn("updates-beta", r.stdout)

    def test_a_file_signed_for_another_build_is_refused(self):
        d = self.folder("0.2.0-dev.29", beta=3)
        app = d / "linux/kiln-launcher_0.2.0-dev.29-beta.3_amd64.AppImage"
        self.key.sign(app, "0.2.0-dev.29")  # the final's id on a test build's file
        run(sys.executable, "-I", HERE / "lib/release.py", "sums", d)
        r = self.script("publish.sh", d, "--host", "example-host", "--dry-run")
        self.assertNotEqual(r.returncode, 0, r.stdout)


if __name__ == "__main__":
    unittest.main()
