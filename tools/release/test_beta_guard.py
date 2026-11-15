"""Offline publication decision; does not run publish.sh or contact either host."""
import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location('beta_guard', Path(__file__).parent / 'lib/beta_guard.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
class Guard(unittest.TestCase):
    def test_both_hosts_must_be_proven_not_newer(self):
        target = '0.2.0-dev.29'
        for hosts, expected in [(['0.2.0-dev.28', target], True), ([target, target], True),
            (['0.2.0-dev.30+beta.1', target], False), ([target, '0.2.0-dev.30+beta.1'], False),
            ([None, target], False), (['invalid JSON', target], False), (['1.0.0-dev..1', target], False)]:
            self.assertEqual(guard.decide(target, hosts)[0], expected, hosts)
        self.assertTrue(guard.decide(target, [None, None], force=True)[0])
        self.assertFalse(guard.decide(target, [target, target], disabled=True, force=True)[0])

    def test_unreadable_and_invalid_json_cli_never_moves_either_host(self):
        import tempfile
        import subprocess
        import sys
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'github.json').write_text('{"version":"0.2.0-dev.29"}')
            for text in [None, '{invalid', '{"version":"garbage"}', '{"version":"0.2.0-dev.29+beta.x"}']:
                path = root / 'frostdev.json'
                if text is not None:
                    path.write_text(text)
                result = subprocess.run([sys.executable, str(Path(__file__).parent / 'lib/beta_guard.py'), '0.2.0-dev.29', str(path), str(root / 'github.json')], capture_output=True, text=True)
                self.assertEqual(result.returncode, 1)
                self.assertIn('BETA CHANNEL NOT MOVED', result.stdout)
