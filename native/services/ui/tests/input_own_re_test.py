#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline checks of the game input paths kiln.ui.input relies on (docs/re/input-ownership.md, 0.2.0-dev.31.13); no game
process. kiln.ui.input adds no game hook of its own (no sigdb entry): it keeps an owned press's release from the game
through kiln.ui's window subclass, its hook on the game's GetRawInputData import slot and its XInputGetState hook. With
the client exe installed (KILN_CLIENT_EXE, or Steam's default folder; read as a file, never run) and on 26430625
(5d9ef2f1), this checks that those are the game's only ways to learn a key or button went up or is held:

1. USER32!GetRawInputData is imported (slot RVA 0x13CFB38) and both of the game's calls go through that slot; no
   GetRawInputBuffer, GetKeyState, GetAsyncKeyState or GetKeyboardState is imported, nor named anywhere in the file.
2. XInput is loaded by name (its DLL and export names are strings); DirectInput8Create is imported (pads kiln.ui does not
   see, a follow-up).
3. The follow-ups' sites from XHL's reference hold: the normalized input dispatch (observation 0x85C35A, dispatch call
   0x85C35F -> 0xEC2B30, mode call 0x85C36B -> 0xE92CC0, UI consumer 0x85C3EE -> 0xA4FC90 whose AL picks the handled
   branch's mov al,1 at 0x85C45B), each unique as a pattern; EmberCodex's hotbar page site (sub bl,8 / and bl,0Fh /
   movzx r8d,bl at 0x3AF7FB..0x3AF809), unique.
"""
import hashlib
import os
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(root / 'tools/sigport'))
from peimg import Image  # noqa: E402

checks = 0


def check(ok, text):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(text)
    print('PASS ' + text, flush=True)


exe = Path(os.environ.get('KILN_CLIENT_EXE') or
           Path.home() / '.local/share/Steam/steamapps/common/Enshrouded/enshrouded.exe')
if not exe.is_file():
    print('input ownership RE: no client exe, checks skipped')
    sys.exit(0)
raw = exe.read_bytes()
if hashlib.sha256(raw).hexdigest() != '5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77':
    print('input ownership RE: the installed client is not 26430625, checks skipped')
    sys.exit(0)
image = Image(exe)
img = image.img


def imports():
    """{dll: {name: iat slot rva}} from the import directory."""
    pe = struct.unpack_from('<I', raw, 0x3C)[0]
    opt = pe + 24
    import_rva = struct.unpack_from('<I', raw, opt + 112 + 8)[0]  # PE32+ (headers: the file's bytes): directories at +112, [1] imports
    out = {}
    d = import_rva
    while True:
        oft, _, _, name_rva, ft = struct.unpack_from('<IIIII', img, d)
        if name_rva == 0:
            break
        dll = img[name_rva:img.index(b'\0', name_rva)].decode().lower()
        names = {}
        i = 0
        while True:
            thunk = struct.unpack_from('<Q', img, (oft or ft) + 8 * i)[0]
            if thunk == 0:
                break
            if not thunk >> 63:
                n = img[thunk + 2:img.index(b'\0', thunk + 2)].decode()
                names[n] = ft + 8 * i
            i += 1
        out[dll] = names
        d += 20
    return out


imp = imports()
user32 = imp.get('user32.dll', {})
check(user32.get('GetRawInputData') == 0x13CFB38, 'USER32!GetRawInputData is imported at slot RVA 0x13CFB38')
every = {n for names in imp.values() for n in names}
for name in ('GetRawInputBuffer', 'GetKeyState', 'GetAsyncKeyState', 'GetKeyboardState'):
    check(name not in every and name.encode() + b'\0' not in raw,
          f'{name} is neither imported nor named in the file (no other way to read held keys)')


def call_slot(at):
    """The slot of an indirect call `FF 15 rel32` at RVA `at`."""
    if img[at:at + 2] != b'\xff\x15':
        return None
    return at + 6 + struct.unpack_from('<i', img, at + 2)[0]


def call_target(at):
    """The target of a direct call `E8 rel32` at RVA `at`."""
    if img[at] != 0xE8:
        return None
    return at + 5 + struct.unpack_from('<i', img, at + 1)[0]


callers = [i for i in range(image.text_va, image.text_va + len(image.text) - 6)
           if img[i] == 0xFF and img[i + 1] == 0x15 and call_slot(i) == 0x13CFB38]
check(callers == [0xFA37FC, 0xFA38E8], f'the game calls GetRawInputData twice, through the slot: {[hex(c) for c in callers]}')
check(img[0xFA37F7:0xFA37FC] == b'\xba\x03\x00\x00\x10' and img[0xFA38E0:0xFA38E5] == b'\xba\x03\x00\x00\x10',
      'both calls read RID_INPUT (mov edx,10000003h): the size query, then the record')

check(imp.get('dinput8.dll', {}).get('DirectInput8Create') is not None, 'DINPUT8!DirectInput8Create is imported (pads kiln.ui does not see)')
for s in (b'XInput1_4.dll\0', b'XInput1_3.dll\0', b'XInput9_1_0.dll\0', b'XInputGetState\0'):
    check(s in raw, f'{s[:-1].decode()} is a string: XInput loads by name (kiln.ui hooks its XInputGetState)')

# the normalized input dispatch (XHL "Observe normalized controller input", "Route an overlay's controller release")
check(img[0x85C35A:0x85C35F] == bytes.fromhex('488d542420') and call_target(0x85C35F) == 0xEC2B30,
      'observation 0x85C35A: lea rdx,[rsp+20h] before the dispatch call 0x85C35F -> 0xEC2B30')
check(img[0x85C364:0x85C36B] == bytes.fromhex('488b8bf8050000') and call_target(0x85C36B) == 0xE92CC0,
      'the same receiver [rbx+5F8h] reloaded, then the mode call 0x85C36B -> 0xE92CC0')
check(img[0x85C3E9:0x85C3EE] == bytes.fromhex('488d542420') and call_target(0x85C3EE) == 0xA4FC90 and
      img[0x85C3F3:0x85C3F7] == bytes.fromhex('84c07564') and 0x85C3F7 + 0x64 == 0x85C45B and img[0x85C45B:0x85C45D] == b'\xb0\x01',
      'UI consumer 0x85C3EE -> 0xA4FC90 on the stack event; test al,al / jne to mov al,1 at 0x85C45B (handled)')
observe = image.find('48 8D 54 24 20 E8 ?? ?? ?? ?? 48 8B 8B F8 05 00 00 E8 ?? ?? ?? ?? 48 8B 8B B0 05 00 00 3C 01 88 83 73 4F 03 00')
check(observe == [0x85C35A], f'the observation run is unique: {[hex(x) for x in observe]}')
route = image.find('48 8D 54 24 20 E8 ?? ?? ?? ?? 84 C0 75 ?? 44 0F B6 44 24 2C 48 8B 83 A0 52 03 00')
check(route == [0x85C3E9], f'the routing run is unique: {[hex(x) for x in route]}')
# EmberCodex's hotbar page (XHL "Suppress one mapped hotbar page change on modifier release")
hotbar = image.find('80 EB 08 48 8D 8C 24 ?? ?? ?? ?? 80 E3 0F 44 0F B6 C3')
check(hotbar == [0x3AF7FB], f'the hotbar page step is unique at 0x3AF7FB: {[hex(x) for x in hotbar]}')

print(f'input ownership RE: {checks} passed')
