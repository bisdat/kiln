#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write tests/fixtures/player-storage/client-input-types.json: the game's reflection of PlayerInput and
ServerConsumedPlayerInput, for the player storage client guard's host test (storage_client_guard_test.cpp).

    python3 -I native/services/items/tests/storage_client_types.py <kfc types dump>...

Each argument is a `kfc types` dump (tools/kfc) of one approved build, read from a read-only exe copy. The fixture keeps
the types the guard's walk reaches: PlayerInput and ServerConsumedPlayerInput with their fields, the type of every field
of PlayerInput.fromClient with its fields, and the type of each of those fields (size, kind and count only). It holds
names, sizes and offsets only, no game code or data.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
OUT = ROOT / 'tests/fixtures/player-storage/client-input-types.json'
STRUCT = 18


def build(dump):
    types = {t['name']: t for t in dump['types']}
    keep, with_fields = {}, set()

    def add(name, fields):
        t = types[name]
        keep[name] = t
        if fields and t['kind'] == STRUCT:
            with_fields.add(name)

    for top in ('keen::ecs::PlayerInput', 'keen::ecs::ServerConsumedPlayerInput'):
        add(top, True)
        for f in types[top]['fields']:
            add(f['type'], False)
    group = next(f['type'] for f in types['keen::ecs::PlayerInput']['fields'] if f['name'] == 'fromClient')
    add(group, True)
    for member in types[group]['fields']:
        add(member['type'], True)
        for f in types[member['type']].get('fields', []):
            add(f['type'], False)
    for name in list(with_fields):
        for f in types[name]['fields']:
            add(f['type'], name in with_fields and f['type'] in with_fields)
    out = {}
    for name in sorted(keep):
        t = keep[name]
        row = [t['size'], t['kind'], t['count']]
        if name in with_fields:
            row.append([[f['name'], f['type'], f['offset']] for f in t['fields']])
        out[name] = row
    return {'exe_sha256': dump['exe_sha256'], 'types': out}


def main(paths):
    builds = [build(json.loads(Path(p).read_text())) for p in paths]
    lines = ['{', '  "format": "name: [size, kind, count, fields?]; a field is [name, type, offset]",', '  "builds": [']
    for i, b in enumerate(builds):
        lines.append('    {"exe_sha256": "%s", "types": {' % b['exe_sha256'])
        rows = ['      %s: %s' % (json.dumps(k), json.dumps(v, separators=(',', ':'))) for k, v in b['types'].items()]
        lines.append(',\n'.join(rows))
        lines.append('    }}' + (',' if i + 1 < len(builds) else ''))
    lines += ['  ]', '}', '']
    OUT.write_text('\n'.join(lines))
    print(f'{OUT.relative_to(ROOT)}: {len(builds)} builds, {sum(len(b["types"]) for b in builds)} types')


if __name__ == '__main__':
    main(sys.argv[1:])
