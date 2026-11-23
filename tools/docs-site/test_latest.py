#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The latest alias of the API reference: latest.py, and its use in publish.sh --sdk-site (/kiln/api/).

  python3 -I tools/docs-site/test_latest.py
"""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
RELEASE = HERE.parent / "release"
PUBLISH = RELEASE / "publish.sh"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


latest = load("latest", HERE / "latest.py")


class Decide(unittest.TestCase):
    def test_a_newer_or_the_same_release_moves_the_alias(self):
        for current in ("", "downloads/0.2.0-dev.30.5/sdk/api", "0.2.0-dev.30.5", "downloads/0.2.0-dev.30.5/sdk/api/", "garbage here"):
            with self.subTest(current=current):
                self.assertTrue(latest.newer("0.2.0-dev.30.6", current)[0])
        self.assertTrue(latest.newer("0.2.0-dev.30.5", "downloads/0.2.0-dev.30.5/sdk/api")[0])

    def test_an_older_release_leaves_it(self):
        move, why = latest.newer("0.2.0-dev.30.2", "downloads/0.2.0-dev.30.5/sdk/api")
        self.assertFalse(move)
        self.assertIn("0.2.0-dev.30.5", why)

    def test_releases_order_as_numbers_and_a_stable_release_is_newer_than_its_dev_builds(self):
        self.assertFalse(latest.newer("0.2.0-dev.9", "0.2.0-dev.10")[0])
        self.assertTrue(latest.newer("0.2.0-dev.31", "0.2.0-dev.30.5")[0])
        self.assertTrue(latest.newer("0.2.0", "0.2.0-dev.31")[0])
        self.assertFalse(latest.newer("0.2.0-dev.31", "0.2.0")[0])

    def test_command_line_exit_codes(self):
        def run(*args):
            return subprocess.run([sys.executable, "-I", str(HERE / "latest.py"), *args], capture_output=True, text=True).returncode
        self.assertEqual(run("newer", "0.2.0-dev.31", "downloads/0.2.0-dev.30.5/sdk/api"), 0)
        self.assertEqual(run("newer", "0.2.0-dev.30.2", "downloads/0.2.0-dev.30.5/sdk/api"), 1)
        self.assertEqual(run("newer", "0.2.0-dev.31"), 0)
        self.assertEqual(run("newer", "not-a-release"), 2)
        self.assertNotEqual(run(), 0)


class InPublish(unittest.TestCase):
    """The function of publish.sh, with ssh and curl stubbed."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "repo/tools").mkdir(parents=True)
        shutil.copytree(HERE, self.root / "repo/tools/docs-site", ignore=shutil.ignore_patterns("__pycache__"))
        (self.root / "repo/tools/release/lib").mkdir(parents=True)
        shutil.copy2(RELEASE / "lib/apihistory.py", self.root / "repo/tools/release/lib/apihistory.py")
        self.dir = self.root / "folder"
        (self.dir / "sdk/api").mkdir(parents=True)
        (self.dir / "sdk/api/index.html").write_text("built")
        text = PUBLISH.read_text()
        start = text.index("latest_api() {")
        self.function = text[start:text.index("\n}\n", start) + 3]

    def run_function(self, name, current, dry=0, curl_ok=True):
        log = self.root / "log"
        log.write_text("")
        script = f'''
set -euo pipefail
dir={self.dir} name={name} host=example-host remote=/srv/kiln dry={dry} repo_dir={self.root / "repo"}
r() {{ if ((dry)); then printf 'would run:'; printf ' %q' "$@"; echo; else "$@"; fi; }}
ssh() {{ echo "ssh $*" >> {log}; if [[ $2 == readlink ]]; then printf '%s' "$CURRENT"; fi; }}
curl() {{ {"true" if curl_ok else "false"}; }}
{self.function}
latest_api
'''
        r = subprocess.run(["bash", "-c", script], capture_output=True, text=True, env={**os.environ, "CURRENT": current})
        return r, log.read_text()

    def test_the_alias_moves_to_a_newer_release_in_one_rename(self):
        r, log = self.run_function("0.2.0-dev.31", "downloads/0.2.0-dev.30.5/sdk/api")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("ssh example-host readlink /srv/kiln/api", log)
        self.assertIn("ssh example-host ln -sfn downloads/0.2.0-dev.31/sdk/api /srv/kiln/.api.new && mv -T /srv/kiln/.api.new /srv/kiln/api", log)
        self.assertIn("https://frostdev.io/kiln/api/ is 0.2.0-dev.31", r.stdout)

    def test_the_first_publication_creates_it(self):
        r, log = self.run_function("0.2.0-dev.30.5", "")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("ln -sfn downloads/0.2.0-dev.30.5/sdk/api", log)

    def test_an_older_release_leaves_the_alias_alone(self):
        r, log = self.run_function("0.2.0-dev.30.2", "downloads/0.2.0-dev.30.5/sdk/api")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("ln -sfn", log)
        self.assertIn("/kiln/api/ is 0.2.0-dev.30.5, newer than 0.2.0-dev.30.2: /kiln/api/ stays", r.stdout)

    def test_a_dry_run_prints_the_rename_and_asks_the_host_nothing(self):
        r, log = self.run_function("0.2.0-dev.31", "downloads/0.2.0-dev.30.5/sdk/api", dry=1)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(log, "")
        self.assertIn("would run: ssh example-host ln\\ -sfn\\ downloads/0.2.0-dev.31/sdk/api", r.stdout)

    def test_a_site_nginx_does_not_serve_yet_is_a_warning_naming_the_location(self):
        r, _ = self.run_function("0.2.0-dev.31", "", curl_ok=False)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("is not served", r.stderr)
        self.assertIn("/kiln/api/", r.stderr)

    def test_a_folder_without_the_reference_changes_nothing(self):
        (self.dir / "sdk/api/index.html").unlink()
        r, log = self.run_function("0.2.0-dev.31", "")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(log, "")
        self.assertIn("stays where it was", r.stdout)


class DryRun(unittest.TestCase):
    """publish.sh itself, a dry run on a made-up release folder (the harness of tools/release/test_beta.py)."""

    def setUp(self):
        beta = load("test_beta", RELEASE / "test_beta.py")
        self.h = beta.Scripts("test_a_release_moves_dev_and_beta_unless_told")
        self.h.setUp()
        self.addCleanup(self.h.tearDown)
        # the harness copies tools/release only: the alias decision is in tools/docs-site
        (self.h.repo / "tools/docs-site").mkdir(parents=True)
        shutil.copy2(HERE / "latest.py", self.h.repo / "tools/docs-site/latest.py")

    def folder(self, version, beta=None, with_api=True):
        d = self.h.folder(version, beta=beta)
        if with_api:
            (d / "sdk/api").mkdir(parents=True)
            (d / "sdk/api/index.html").write_text("built")
            subprocess.run([sys.executable, "-I", str(RELEASE / "lib/release.py"), "sums", str(d)], check=True, capture_output=True)
        return d

    def publish(self, d, *args):
        return self.h.script("publish.sh", d, "--host", "example-host", "--dry-run", *args)

    def test_a_release_with_sdk_site_moves_the_alias(self):
        r = self.publish(self.folder("0.2.0-dev.29"), "--sdk-site")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("would read which release example-host:/opt/frostdev-downloads/kiln/api names", r.stdout)
        self.assertIn("ln\\ -sfn\\ downloads/0.2.0-dev.29/sdk/api\\ /opt/frostdev-downloads/kiln/.api.new\\ \\&\\&\\ mv\\ -T", r.stdout)
        self.assertLess(r.stdout.index("--chmod=D755"), r.stdout.index(".api.new"))           # after the upload of the folder

    def test_without_sdk_site_the_alias_stays(self):
        r = self.publish(self.folder("0.2.0-dev.29"))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn(".api.new", r.stdout)

    def test_a_test_build_never_moves_it(self):
        r = self.publish(self.folder("0.2.0-dev.29", beta=3, with_api=False), "--sdk-site")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn(".api.new", r.stdout)


class Wiring(unittest.TestCase):
    def test_publish_calls_it_for_a_release_with_sdk_site_only(self):
        text = PUBLISH.read_text()
        self.assertIn("if ((sdk_site)) && [[ $channel != beta ]]; then latest_api; fi", text)
        # defined before the first line that runs it
        self.assertLess(text.index("latest_api() {"), text.index("; then latest_api; fi"))


if __name__ == "__main__":
    unittest.main()
