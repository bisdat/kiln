#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline checks of chat history's sigdb definitions (sigdb/symbols/client.d/chatui.json); no game process.

1. Every sigdb name in chat_input_probe.cpp is gated by the history or
   completion feature. Completion-only node/cursor offsets must not gate history.
2. With the client exe installed (KILN_CLIENT_EXE, or Steam's default folder; read as a file, never run): both
   features resolve, broken completion captures disable only completion, and
   on 26430625 (5d9ef2f1) the verified offset values match the executable.
"""
import copy
import hashlib
import json
import os
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(root / 'tools/sigport'))
from peimg import Image  # noqa: E402
from sigdb_check import Resolver  # noqa: E402

doc = json.loads((root / 'sigdb/symbols/client.d/chatui.json').read_text())
history_feature = doc['features']['chat.ui.input']
completion_feature = doc['features']['chat.ui.completion']
gated = set(history_feature['symbols']) | set(history_feature['constants'])
gated |= set(completion_feature['symbols']) | set(completion_feature['constants'])
assert 'chatui/widget.node' not in history_feature['constants']
assert 'chatui/edit.cursor' not in history_feature['constants']
code = (root / 'native/services/ui/chat_input_probe.cpp').read_text()
used = set(re.findall(r'"(chatui/[A-Za-z0-9_.]+)"', code))
assert used, 'chat_input_probe.cpp names no chatui symbol'
assert used <= gated, f'read but not gated by chat feature: {sorted(used - gated)}'
for name in gated & set(doc['constants']):
    assert name in doc['constants'], f'{name}: listed in the feature, not declared'

exe = Path(os.environ.get('KILN_CLIENT_EXE') or
           Path.home() / '.local/share/Steam/steamapps/common/Enshrouded/enshrouded.exe')
if not exe.is_file():
    print('chat_sigdb: names gated; no client exe, resolve checks skipped')
    sys.exit(0)
image = Image(exe)


def resolve(d):
    return Resolver(image, d).run()


_, consts, feats = resolve(copy.deepcopy(doc))
assert feats['chat.ui.input'], 'chat.ui.input does not resolve on ' + str(exe)
assert feats['chat.ui.completion'], 'chat.ui.completion does not resolve on ' + str(exe)

# A changed layout: the strategy that captures state.editing no longer matches, so the feature is off (never a guess).
changed = copy.deepcopy(doc)
for f in changed['symbols']['@chatui/TextInputEdit.fields']['find']:
    if '{chatui/state.editing:' in f['pattern']:
        f['pattern'] = f['pattern'].replace('41 F6 DC', '41 F6 DD')
assert not resolve(changed)[2]['chat.ui.input'], 'a missing layout capture must turn chat.ui.input off'
# Node layout must be captured, not assumed. A broken capture disables only
# completion; existing history is still safe and independently gated.
changed_node = copy.deepcopy(doc)
changed_node['symbols']['@chatui/TextInputState.node']['find'][0]['pattern'] = (
    changed_node['symbols']['@chatui/TextInputState.node']['find'][0]['pattern']
    .replace('40 53 48 83', '40 53 49 83'))
changed_node_feats = resolve(changed_node)[2]
assert changed_node_feats['chat.ui.input'], 'history must survive a missing widget-node capture'
assert not changed_node_feats['chat.ui.completion'], 'a missing widget-node capture must turn completion off'

changed_cursor = copy.deepcopy(doc)
changed_cursor['symbols']['@chatui/EditSetText.cursor']['find'][0]['pattern'] = (
    changed_cursor['symbols']['@chatui/EditSetText.cursor']['find'][0]['pattern']
    .replace('8B 4F 08', '8B 4E 08'))
changed_cursor_feats = resolve(changed_cursor)[2]
assert changed_cursor_feats['chat.ui.input'], 'history must survive a missing editor-cursor capture'
assert not changed_cursor_feats['chat.ui.completion'], 'a missing editor-cursor capture must turn completion off'

sha = hashlib.sha256(exe.read_bytes()).hexdigest()
if sha.startswith('5d9ef2f1'):
    want = {'chatui/state.hash.lo': 0x968a8b89, 'chatui/state.hash.hi': 0x72615e88, 'chatui/state.edit': 0x10,
            'chatui/state.editing': 0x1988, 'chatui/widget.buffer': 0x150,
            'chatui/widget.node': 0x08, 'chatui/edit.cursor': 0x1938}
    for name, value in want.items():
        assert consts[name]['ok'] and consts[name]['value'] == value, f'{name}: {consts[name]}'
print(f'chat_sigdb: all checks passed (exe {sha[:8]})')
