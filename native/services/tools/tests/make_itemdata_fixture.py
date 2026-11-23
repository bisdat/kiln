#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reduce an independently extracted game TypeDb to the fields used by the native tool writer test.
No game data is committed. Pass the TypeDb JSON and an output JSON, both in assigned scratch storage.
"""
import json
import sys
from pathlib import Path

source = json.loads(Path(sys.argv[1]).read_text())
types = {t['name']: t for t in source['types']}
keep = {
    'keen::ItemInfo': {'equipment', 'sequences'},
    # Keep two fields: the production flattener correctly treats a one-field struct as a scalar wrapper.
    'keen::EquipmentSetup': {'slot', 'repeatActionWhenPressed'},
    'keen::ItemSequence': {'sequenceId', 'sequence', 'triggeredBy'},
    'keen::actor::ActorSequenceResource': set(),
}
selected = {}
def visit(name):
    if not name or name in selected:
        return
    t = types[name]
    fields = t.get('fields', [])
    if name in keep:
        fields = [f for f in fields if f['name'] in keep[name]]
    selected[name] = {k: t[k] for k in ('name', 'size', 'kind', 'hash', 'inner')}
    selected[name]['fields'] = fields
    visit(t.get('inner'))
    for f in fields:
        visit(f['type'])
visit('keen::ItemInfo')
def offset(name, field):
    return next(f['offset'] for f in types[name]['fields'] if f['name'] == field)
result = {
    'types': list(selected.values()),
    'sequencesOffset': offset('keen::ItemInfo', 'sequences'),
    'sequenceSize': types['keen::ItemSequence']['size'],
    'referenceOffset': offset('keen::ItemSequence', 'sequence'),
    'idOffset': offset('keen::ItemSequence', 'sequenceId'),
}
Path(sys.argv[2]).write_text(json.dumps(result))
print(f"Tool writer reflection fixture: {len(selected)} real reflected types")
