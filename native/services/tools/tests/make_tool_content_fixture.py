#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reduce an independently extracted game TypeDb to what the tool content check reads: ItemInfo's equipment, sequences,
tags and impactValues (every type below them intact, with the game's offsets), and ItemRegistryResource's itemTags.
No game data is committed. Pass the TypeDb JSON and an output JSON, both in assigned scratch storage.
"""
import json
import sys
from pathlib import Path

source = json.loads(Path(sys.argv[1]).read_text())
types = {t['name']: t for t in source['types']}
keep = {
    'keen::ItemInfo': {'equipment', 'sequences', 'tags', 'impactValues'},
    'keen::ItemRegistryResource': {'itemTags'},
    'keen::actor::ActorSequenceResource': set(),
}
selected = {}


def visit(name):
    if not name or name in selected:
        return
    t = types[name]
    fields = t.get('fields') or []
    if name in keep:
        fields = [f for f in fields if f['name'] in keep[name]]
    selected[name] = {k: t[k] for k in ('name', 'size', 'kind', 'hash', 'inner')}
    selected[name]['fields'] = fields
    visit(t.get('inner'))
    for f in fields:
        visit(f['type'])


for root in keep:
    visit(root)
# the variant elements of impact configurations (BlobVariant names its element types by hash, not by field)
for name, t in types.items():
    if name.startswith('keen::impact::') and name.endswith('Config') and t.get('kind') == 18:
        visit(name)
Path(sys.argv[2]).write_text(json.dumps({'types': list(selected.values())}))
print(f"Tool content reflection fixture: {len(selected)} real reflected types")
