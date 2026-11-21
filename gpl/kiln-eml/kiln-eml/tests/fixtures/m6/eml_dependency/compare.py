# SPDX-License-Identifier: GPL-3.0-or-later
"""Strict shared-v2 comparator; never compares or strips dependency provenance."""
import json
import sys
from pathlib import Path


def validate(value):
    if value.get('version') != 2 or not isinstance(value.get('records'), list):
        raise ValueError('shared-v2 transcript required')
    if 'fixture_error' in value:
        raise ValueError('fixture failed; not parity')
    if not value['records']:
        raise ValueError('empty transcript; no observations')
    for seq, record in enumerate(value['records'], 1):
        if record.get('seq') != seq or not isinstance(record.get('label'), str):
            raise ValueError('invalid record identity')
        if record.get('kind') == 'expect' and record.get('pass') is not True:
            raise ValueError('failed fixture assertion')


def compare(old, candidate):
    validate(old)
    validate(candidate)
    # Sorted JSON keys only: preserve every field, message byte, array order, scalar type and arity.
    canonical = lambda value: json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(',', ':'))
    if canonical(old) != canonical(candidate):
        raise ValueError('Lua-visible differential mismatch')


if __name__ == '__main__':
    try:
        if len(sys.argv) == 3:
            old = json.loads(Path(sys.argv[1]).read_text())
            candidate = json.loads(Path(sys.argv[2]).read_text())
        elif len(sys.argv) == 1:
            inputs = json.load(sys.stdin)
            old, candidate = inputs['old'], inputs['candidate']
        else:
            raise ValueError('usage: compare.py [old.json candidate.json]')
        compare(old, candidate)
        print(f"identical: {len(old['records'])} shared-v2 records")
    except (ValueError, KeyError, OSError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
