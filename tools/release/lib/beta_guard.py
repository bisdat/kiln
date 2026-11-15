"""Fail-closed decision for both published beta pointers. No network or shell calls."""
import argparse
import json
from pathlib import Path
import re
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from release import build_order


def valid_version(v):
    if not isinstance(v, str) or len(v) > 256:
        return False
    m = re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z.-]+))?(?:\+([0-9A-Za-z.-]+))?', v)
    if not m:
        return False
    if m[5] and (m[5] == "beta" or m[5].startswith("beta.")) and not re.fullmatch(r"beta\.[1-9][0-9]{0,8}", m[5]):
        return False
    for group in (m[4], m[5]):
        if group is not None and any(not p for p in group.split('.')):
            return False
    return not m[4] or all(not (p.isdigit() and len(p) > 1 and p[0] == '0') for p in m[4].split('.'))


def decide(target, live, force=False, disabled=False):
    if disabled:
        return False, 'Beta channel stays: --no-beta-channel.'
    if force:
        return True, 'WARNING: forcing both beta pointers regardless of their live versions.'
    if not valid_version(target) or len(live) != 2 or not all(valid_version(v) for v in live):
        return False, 'WARNING: BETA CHANNEL NOT MOVED: both hosts could not be read, parsed and compared. Inspect both pointers or use --force-beta-channel explicitly.'
    if any(build_order(v, target) > 0 for v in live):
        return False, f'WARNING: BETA CHANNEL NOT MOVED: a host names a newer build ({", ".join(live)}).'
    return True, 'Both beta pointers are no newer than the release.'


def read_manifest(path):
    try:
        return json.loads(Path(path).read_text())['version']
    except (OSError, ValueError, KeyError, TypeError):
        return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument('target')
    p.add_argument('frostdev')
    p.add_argument('github')
    p.add_argument('--force', action='store_true')
    a = p.parse_args()
    move, reason = decide(a.target, [read_manifest(a.frostdev), read_manifest(a.github)], force=a.force)
    print(reason)
    return 0 if move else 1

if __name__ == '__main__':
    raise SystemExit(main())
