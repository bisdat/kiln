#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only, build-specific diagnostic for issue #44's HUD/Social chat call sites.

Only examines enshrouded.exe bytes. It neither edits the game nor hooks input.
Produces scoped, unique candidate sigdb patterns to review before changing the
fork's chatui.json; it does NOT automatically install or change signatures.
"""
import argparse
import hashlib
import re
import struct
from pathlib import Path

EXPECTED_SHA256 = "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77"
INPUT_RVA = 0xD2A170
CALLS = (
    ("HudInputReturn", "HudChatWindow", 0xCDC320, 0xCDD674, 8192),
    ("SocialInputReturn", "SocialChat", 0xCBE6D0, 0xCC0777, 16384),
)


def text_section(exe):
    if exe[:2] != b"MZ":
        raise ValueError("not a PE executable (missing MZ header)")
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    if exe[pe:pe + 4] != b"PE\x00\x00":
        raise ValueError("not a PE executable (missing PE header)")
    n = struct.unpack_from("<H", exe, pe + 6)[0]
    opt_size = struct.unpack_from("<H", exe, pe + 20)[0]
    start = pe + 24 + opt_size
    for i in range(n):
        off = start + i * 40
        name = exe[off:off + 8].split(b"\x00")[0]
        if name == b".text":
            vsize, rva, raw_size, raw_ptr = struct.unpack_from("<IIII", exe, off + 8)
            if raw_ptr + raw_size > len(exe):
                raise ValueError("truncated .text section")
            return rva, exe[raw_ptr:raw_ptr + min(raw_size, vsize)]
    raise ValueError("no .text section")


def make_pattern(data, center, lo, hi):
    # Rel32 slots are deliberately wildcards: the surrounding bytes must
    # identify the call uniquely, as in Kiln's signature resolver.
    for total in range(1, 49):
        for before in range(min(total, 24) + 1):
            after = total - before
            if after > 24 or center - before < lo or center + 5 + after > hi:
                continue
            pre = data[center - before:center]
            post = data[center + 5:center + 5 + after]
            rx = re.compile(re.escape(pre) + b"\xe8.{4}" + re.escape(post), re.S)
            hits = [m.start() + before for m in rx.finditer(data, lo, hi)]
            if hits == [center]:
                hex_bytes = lambda b: " ".join(f"{x:02X}" for x in b)
                return " ".join(filter(None, [
                    hex_bytes(pre),
                    "E8 {chatui/TextInputProcess:rel32}",
                    hex_bytes(post),
                ])), before + 5
    raise ValueError("cannot make a unique pattern in 48 context bytes")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("exe", type=Path, help="path to enshrouded.exe")
    args = p.parse_args()
    exe = args.exe.read_bytes()
    sha = hashlib.sha256(exe).hexdigest()
    print("exe:", args.exe)
    print("sha256:", sha)
    if sha != EXPECTED_SHA256:
        raise SystemExit("STOP: executable is not the client build used by issue #44")
    text_rva, code = text_section(exe)
    print(f".text: RVA 0x{text_rva:x}, {len(code)} bytes")
    for name, scope_name, function_rva, call_rva, scope_size in CALLS:
        off = call_rva - text_rva
        lo = function_rva - text_rva
        hi = min(len(code), lo + scope_size)
        if off < lo or off + 5 > hi or code[off] != 0xE8:
            raise SystemExit(f"STOP: {name} expected CALL missing at RVA 0x{call_rva:x}")
        rel = struct.unpack_from("<i", code, off + 1)[0]
        target = call_rva + 5 + rel
        print(f"\n{name}: CALL 0x{call_rva:x} -> 0x{target:x}")
        if target != INPUT_RVA:
            raise SystemExit(f"STOP: {name} calls unexpected target (expected 0x{INPUT_RVA:x})")
        calls = []
        for pos in range(lo, hi - 4):
            if code[pos] == 0xE8:
                to = text_rva + pos + 5 + struct.unpack_from("<i", code, pos + 1)[0]
                if to == INPUT_RVA:
                    calls.append(text_rva + pos)
        print("direct calls to TextInputProcess in scope:",
              ", ".join(f"0x{x:x}" for x in calls) or "(none)")
        pattern, offset = make_pattern(code, off, lo, hi)
        print(f"  Pattern: {pattern}")
        print(f"  Offset: {offset}")
        print(f"  Scope: chatui/{scope_name} ({scope_size} bytes)")
    print("\nCandidate patterns only. No files have been changed.")


if __name__ == "__main__":
    main()
