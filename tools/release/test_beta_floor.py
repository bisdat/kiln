"""Offline beta cut metadata; never invokes a release entry-point script."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('release', Path(__file__).parent / 'lib/release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)

class Floor(unittest.TestCase):
    def test_floor_must_be_a_release_older_than_the_beta(self):
        self.assertEqual(release.beta_floor('v0.2.0-dev.28', '0.2.0-dev.29+beta.1'), '0.2.0-dev.28')
        for tag in ['', 'garbage', 'v0.2.0-dev.29-beta.1', 'v0.2.0-dev.30']:
            with self.assertRaises(ValueError):
                release.beta_floor(tag, '0.2.0-dev.29+beta.1')

    def test_linux_container_receives_the_baked_floor(self):
        script = (Path(__file__).parent / 'release.sh').read_text()
        self.assertIn('--env KILN_LAUNCHER_BASE_RELEASE', script)

    def test_manifest_carries_the_floor_as_information(self):
        import argparse
        import tempfile
        import json
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'app').write_bytes(b'fixture')
            (root / 'sig').write_text('fixture-signature')
            args = argparse.Namespace(notes=None, entry=[f'linux-x86_64=https://example.invalid/app|{root / "sig"}|{root / "app"}'],
                version='0.2.0-dev.29+beta.1', beta=1, channel='beta', date=None, page='download', runtime=None,
                out=str(root / 'latest.json'), base_release='0.2.0-dev.28')
            release.cmd_manifest(args)
            self.assertEqual(json.loads((root / 'latest.json').read_text())['kiln']['baseRelease'], '0.2.0-dev.28')

    def test_beta_cut_includes_a_release_tag_on_the_cut_commit(self):
        import re
        import tempfile
        import subprocess
        import os
        with tempfile.TemporaryDirectory() as d:
            def git(*args):
                return subprocess.run(['git', '-C', d, '-c', 'user.name=fixture', '-c', 'user.email=fixture@example.invalid', *args], check=True, capture_output=True)
            git('init', '-q')
            git('commit', '-qm', 'first', '--allow-empty')
            git('tag', 'v0.2.0-dev.28')
            git('commit', '-qm', 'second', '--allow-empty')
            git('tag', 'v0.2.0-dev.29')
            git('tag', 'v0.2.0-dev.30-beta.1')
            script = (Path(__file__).parent / 'release.sh').read_text()
            fn = re.search(r'^previous_release\(\) \{\n.*?^\}', script, re.M | re.S).group()
            for channel, expected in [('beta', 'v0.2.0-dev.29'), ('dev', 'v0.2.0-dev.28')]:
                result = subprocess.run(['bash', '-euo', 'pipefail', '-c', 'repo=$1\n' + fn + '\nprevious_release HEAD "$2"', 'fixture', d, channel], capture_output=True, text=True, env=dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_SYSTEM='/dev/null'))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)
