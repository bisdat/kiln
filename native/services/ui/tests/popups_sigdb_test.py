#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline checks of the popup panels' sigdb definitions (sigdb/symbols/client.d/ui-popups.json, GitHub issue 117); no
game process.

1. Every sigdb name popups.cpp reads is gated by feature ui.popups (or ui.error_popup for the error byte).
2. With the client exe installed (KILN_CLIENT_EXE, or Steam's default folder; read as a file, never run): the feature
   resolves; on 26430625 (5d9ef2f1) the builder, the node rect accessor and the offsets are the qualified ones, and the
   static anchors the code relies on hold (the panel's node is the child frame's second word, the node rect is the
   space of ItemSlot's result rect, the item menu reaches the builder from the backpack and the chest window); a
   changed byte in either pattern turns the feature off.
"""
import copy
import hashlib
import os
import re
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(root / 'tools/sigport'))
from peimg import Image  # noqa: E402
from sides import compose  # noqa: E402
from sigdb_check import Resolver  # noqa: E402

checks = 0


def check(ok, text):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(text)
    print('PASS ' + text, flush=True)


parts = {name: doc for name, doc, _ in compose('client')}
doc = parts['ui-popups.json']
feature = doc['features']['ui.popups']
error_feature = parts['ui.json']['features']['ui.error_popup']
code = (root / 'native/services/ui/popups.cpp').read_text()
used = set(re.findall(r'"(ui/[A-Za-z0-9_.]+)"', code))
gated = set(feature['symbols']) | set(feature['constants'])
error_gated = set(error_feature['symbols']) | set(error_feature['constants'])
check(used and used <= gated | error_gated, f'popups.cpp reads only gated names ({sorted(used)})')
check({'ui/sys.popups', 'ui/popups.error'} & used <= error_gated and not ({'ui/sys.popups', 'ui/popups.error'} & gated),
      'the error popup byte is gated by ui.error_popup alone, not by ui.popups')
check('feat("ui.popups")' in code and 'feat("ui.error_popup")' in code, 'popups.cpp asks for both features')

exe = Path(os.environ.get('KILN_CLIENT_EXE') or
           Path.home() / '.local/share/Steam/steamapps/common/Enshrouded/enshrouded.exe')
if not exe.is_file():
    print(f'popups sigdb: {checks} passed; no client exe, resolve checks skipped')
    sys.exit(0)
raw = exe.read_bytes()
if hashlib.sha256(raw).hexdigest() != '5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77':
    print(f'popups sigdb: {checks} passed; the installed client is not 26430625, anchor checks skipped')
    sys.exit(0)
image = Image(exe)
img = image.img

# the definitions as the core resolves them: ui-popups.json with ui.json's UiNodeRect and GameUiFrame
symbols = copy.deepcopy(doc['symbols'])
for name in ('ui/GameUiFrame',):
    symbols[name] = parts['ui.json']['symbols'][name]
test_doc = {'symbols': symbols, 'constants': doc['constants'], 'features': {'ui.popups': feature}}
found, consts, feats = Resolver(image, copy.deepcopy(test_doc)).run()
check(feats['ui.popups'], 'feature ui.popups resolves on 26430625')
begin = found['ui/PopupWindowBegin']['rva']
node_rect = found['ui/UiNodeRect']['rva']
check(begin == 0xb1c880, 'ui/PopupWindowBegin = 0xb1c880')
check(node_rect == 0x11b67d0, 'ui/UiNodeRect = 0x11b67d0 (the same function ui.json captures)')
sub, panel, rect = (consts[n]['value'] for n in ('ui/popup.subframe', 'ui/popup.panel', 'ui/node.rect'))
check((sub, panel, rect) == (0x1a0, 0x1a8, 0x114), 'subframe 0x1a0, panel 0x1a8, node rect 0x114')
check(panel == sub + 8, 'the panel node is the child frame\'s second word (popups.cpp refuses otherwise)')


def rel(at):
    return at + 5 + struct.unpack_from('<i', img, at + 1)[0]


# UiNodeRect is a plain accessor: lea rax,[rcx+0x114]; ret (Kiln reads the field instead of calling it)
check(img[node_rect:node_rect + 8] == bytes.fromhex('488D8114010000C3'), 'UiNodeRect is lea rax,[rcx+node.rect]; ret')
# the builder opens its window (0xa3f530) at the params' layer, then the child frame (0xa3cbd0) at frame+0x1a0
check(img[begin + 0x27:begin + 0x2e] == bytes.fromhex('458B8874010000') and rel(begin + 0x77) == 0xa3f530,
      'the builder opens its window at params+0x174\'s layer through 0xa3f530')
check(img[begin + 0xaa:begin + 0xb1] == bytes.fromhex('488D8EA0010000') and rel(begin + 0xb1) == 0xa3cbd0,
      'the panel is a child frame at frame+0x1a0 (0xa3cbd0)')
# the child frame constructor puts the new node (0x11c4c60) at its +8
check(rel(0xa3cbfa) == 0x11c4c60 and img[0xa3cbff:0xa3cc06] == bytes.fromhex('488BCB48894308'),
      'the child frame keeps its new layout node at +8')
# it returns the frame (rax = rsi = rcx at entry, the value hk_begin reads when it is set)
check(img[begin + 0x45:begin + 0x48] == bytes.fromhex('488BF1'), 'the builder keeps its frame argument in rsi')
# the builder clamps the panel by the node's previous layout: UiNodeRect(frame+0x1a8) read before the position is set
check(img[0xb1cdeb:0xb1cdf2] == bytes.fromhex('488B8EA8010000') and rel(0xb1cdf2) == node_rect,
      'the builder reads its panel node\'s rect through UiNodeRect')
# ItemSlot's result rect (what Ui.Inventory marks) is UiNodeRect of its slot node: the same space
check(rel(0xc05302) == node_rect and img[0xc05307:0xc0530e] == bytes.fromhex('488B9D60020000') and
      img[0xc05312:0xc05318] == bytes.fromhex('0F10000F1103'),
      'ItemSlot copies its result rect from UiNodeRect: popup and slot rects share one space')
# the item menu: ItemSlot opens it (context +0xb94 = the item) and the backpack and chest window draw it through
# 0xb7bab0, which calls the builder with "ContextMenuWindow"
check(img[0xc052d6:0xc052dd] == bytes.fromhex('49898E940B0000'), 'ItemSlot writes the menu\'s item into context+0xb94')
check(rel(0xb7bb09) == begin, '0xb7bab0 builds its panel through PopupWindowBegin')
name_at = 0xb7bad9 + struct.unpack_from('<i', img, 0xb7bad5)[0]
check(img[name_at:name_at + 18] == b'ContextMenuWindow\0', 'its window is "ContextMenuWindow"')
for call, gate in ((0xb518d2, 0xb51852), (0xb9e04f, 0xb9dfe7)):
    check(rel(call) == 0xb7bab0 and bytes.fromhex('940B0000') in img[gate:gate + 8],
          f'{call:#x}: the item menu is drawn after the context+0xb94 open test at {gate:#x}')

# a changed byte in either pattern turns the feature off
for at in (begin + 0x2, begin + 0xab, 0xb1cdec):
    altered = copy.copy(image)
    b = bytearray(img)
    b[at] ^= 0x01
    altered.img = bytes(b)
    check(not Resolver(altered, copy.deepcopy(test_doc)).run()[2]['ui.popups'], f'a changed byte at {at:#x} turns ui.popups off')
print(f'popups sigdb: {checks} passed, 0 failed')
