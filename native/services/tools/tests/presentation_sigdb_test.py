#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exact-build, read-only material lookup signature checks; never executes game code."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools/sigport"))
from peimg import Image
from sides import compose
from sigdb_check import Resolver, parse
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import re
import struct

SYMBOL = "tools/TerrainConfigLookup"
BUILDS = {
    "client": (26430625, "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77", 0xe9f270),
    "server": (26430253, "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2", 0x9464b0),
}


class MaterialLookupSignatureTest(unittest.TestCase):
    def check_build(self, side):
        path = getattr(self.args, side)
        build, sha, expected = BUILDS[side]
        self.assertEqual(hashlib.sha256(Path(path).read_bytes()).hexdigest(), sha)
        record = json.loads((ROOT / f"sigdb/builds/{side}/{sha}.json").read_text())
        self.assertEqual(record["sha256"], sha)
        self.assertEqual(record["steam_build"], build)
        doc = next(doc for name, doc, _ in compose(side) if name == "tools-presentation.json")
        image = Image(path)
        resolver = Resolver(image, doc)
        for strategy in doc["symbols"][SYMBOL]["find"]:
            hits = resolver.scan(parse(strategy["pattern"])[0], resolver.text_lo, resolver.text_hi, limit=100)
            print(f"{side} {build}: {len(hits)} matches at {[hex(hit) for hit in hits]}", flush=True)
            self.assertEqual(len(hits), 1, f"{side}: lookup anchor must be unique")
            self.assertEqual(hits[0] + strategy.get("offset", 0), expected)
        symbols, _, features = resolver.run()
        self.assertTrue(symbols[SYMBOL]["ok"], symbols[SYMBOL])
        self.assertEqual(symbols[SYMBOL]["rva"], expected)
        self.assertTrue(features["tools.material_lookup"])
        self.assertEqual(record["symbols"][SYMBOL], hex(expected))
        self.assertTrue(record["features"]["tools.material_lookup"])
        self.assertNotEqual(record["services"]["tools-presentation"]["symbols"][SYMBOL], "unresolved")
        # The original-entry guard remains deliberately independent of the discovery signature.
        # Its old prefix also matches the preceding value-returning helper; the new signature
        # extends through the pointer-or-null return, not that helper's field/default return.
        source = (ROOT / "native/services/tools/hooks/presentation.cpp").read_text()
        entry_pattern = source.split('install(lookup_hook,"' + SYMBOL + '","', 1)[1].split('"', 1)[0]
        rx, _, length = parse(entry_pattern)
        self.assertIsNotNone(rx.fullmatch(image.img[expected:expected + length]))
        old_hits = resolver.scan(rx, resolver.text_lo, resolver.text_hi, limit=100)
        self.assertEqual(old_hits, [expected - 0x60, expected])
        print(f"{side} {build}: original prefix has {len(old_hits)} matches; qualified entry {expected:#x}", flush=True)

    def test_client(self):
        self.check_build("client")

    def test_server(self):
        self.check_build("server")

    def test_client_selector_and_brush_frame(self):
        image = Image(self.args.client)
        doc = next(doc for name, doc, _ in compose("client") if name == "tools-presentation.json")
        resolver = Resolver(image, doc)
        expected = {"tools/MaterialSelector": 0x40e5d0, "tools/MaterialPreview": 0xb1a230,
                    "tools/RootIdentity": 0xd3e130, "tools/BrushQueryFrame": 0xc3fe18, "tools/BrushQuery": 0xd25b50}
        for symbol, rva in expected.items():
            for strategy in doc["symbols"][symbol]["find"]:
                hits = resolver.scan(parse(strategy["pattern"])[0], resolver.text_lo, resolver.text_hi, limit=100)
                self.assertEqual([hit + strategy.get("offset", 0) for hit in hits], [rva], symbol)
                print(f"client 26430625: {symbol} has one match at {rva:#x}", flush=True)
        core = json.loads((ROOT / "sigdb/symbols/client.json").read_text())
        core_resolver = Resolver(image, core)
        for strategy in core["symbols"]["GetComponent"]["find"]:
            hits = core_resolver.scan(parse(strategy["pattern"])[0], core_resolver.text_lo, core_resolver.text_hi, limit=100)
            self.assertEqual(hits, [0x9b2ec0])
            print("client 26430625: GetComponent has one match at 0x9b2ec0", flush=True)
        site = expected["tools/BrushQueryFrame"]
        self.assertEqual(image.img[site], 0xe8)
        relative = int.from_bytes(image.img[site+1:site+5], "little", signed=True)
        self.assertEqual(site+5+relative, expected["tools/BrushQuery"])
        # Source of the two original GUID temporaries, independently decoded from the shipped Small brush.
        self.assertEqual(image.img[0x1d8aa68:0x1d8aa78].hex(), "cfa6ae08e8e06c4d8a29a21f4693e006")

    def test_client_selector_halves(self):
        """The material selector's two halves (kiln-tools' tools/MaterialSelectorBody/Done, kiln-items' build/client.
        MaterialRowsBody/Done: the same two sites) and Building Tools 0.34.0's lookup of the function (src/main.cpp
        SIG_STOCKS: one exact match of its first 35 bytes, then the bytes and calls below), on the client."""
        image = Image(self.args.client)
        symbols = {}
        for _, part, _ in compose("client"):
            symbols.update(part.get("symbols", {}))
        found, _, _ = Resolver(image, {"symbols": symbols, "features": {}}).run()
        rva = lambda name: found[name]["rva"] if found[name].get("ok") else self.fail(name + " does not resolve")
        fn, body, done = rva("tools/MaterialSelector"), rva("tools/MaterialSelectorBody"), rva("tools/MaterialSelectorDone")
        self.assertEqual((fn, body, done), (0x40E5D0, fn + 0x23, fn + 0xA8A))
        self.assertEqual((rva("build/client.MaterialRows"), rva("build/client.MaterialRowsBody"), rva("build/client.MaterialRowsDone")),
                         (fn, body, done))
        code = list(Cs(CS_ARCH_X86, CS_MODE_64).disasm(bytes(image.img[fn:fn + 0xAC6]), fn))
        text = lambda i: f"{i.mnemonic} {i.op_str}"
        at = {i.address: i for i in code}
        rets = [i.address for i in code if i.mnemonic == "ret"]
        self.assertEqual(rets, [done + 0x1A], "one ret, right after the epilogue that starts at the second site")
        self.assertEqual(text(at[body]), "lea r15, [rcx + 0x638]")
        self.assertEqual(text(at[done]), "mov rbx, qword ptr [rsp + 0x1d0]")
        before = [i for i in code if i.address < body]
        self.assertFalse([text(i) for i in before if i.mnemonic.startswith("j") or i.mnemonic == "call"],
                         "nothing but the prologue before the first site")
        self.assertFalse([text(i) for i in before if i.op_str.split(",")[0] in ("rcx", "ecx", "rdx", "edx", "r8", "r8d", "r8b")],
                         "rcx (the groups), rdx (the access) and r8b (currentOnly) arrive at the first site as the caller passed them")
        body_code = [i for i in code if body <= i.address < done]
        regs = ("rsi", "esi", "si", "sil")
        self.assertEqual([text(i) for i in body_code if i.op_str.split(",")[0] in regs and i.mnemonic not in ("cmp", "test", "bt", "push")],
                         ["mov rsi, rcx"], "rsi holds the groups from + 0x3b to the second site")
        self.assertFalse([text(i) for i in body_code if i.mnemonic in ("push", "pop") or i.op_str.split(",")[0] == "rsp"],
                         "the function's rsp is the same at both sites")
        # every branch: none lands inside a site's moved bytes or between the second site and the ret
        targets = {int(i.op_str, 16) for i in code if (i.mnemonic.startswith("j")) and i.op_str.startswith("0x")}
        tables = [int(m, 16) for i in code for m in re.findall(r"\+ (0x[0-9a-f]+)\]", i.op_str)
                  if fn < int(m, 16) < fn + 0xC00 and i.mnemonic == "mov" and "*4" in i.op_str]
        self.assertEqual(len(tables), 4, "the four switch tables (RVAs of their cases)")
        entries = set()
        for t in tables:
            for k in range(64):
                v = struct.unpack_from("<I", image.img, t + 4 * k)[0]
                if not fn <= v < fn + 0xAC6:
                    break
                entries.add(v)
        targets |= entries
        moved = [(body, body + 7), (done, done + 8)]
        self.assertFalse([hex(t) for t in targets if any(a < t < b for a, b in moved)], "no branch into a site's moved bytes")
        self.assertFalse([hex(t) for t in targets if done < t <= rets[0]], "no branch past the second site to the ret: every return passes it")
        past = sorted(t for t in targets if t > rets[0])
        self.assertEqual(past, [rets[0] + 1], "the only case past the ret is the switch's fatal default")
        fatal = [i for i in code if i.address >= rets[0] + 1]
        self.assertEqual([i.mnemonic for i in fatal][-2:], ["call", "int3"], "... which never returns")

        # Building Tools 0.34.0: find_unique("Stocks", SIG_STOCKS), then these bytes and calls (hook_calls redirects the
        # callers and the call at + 0xa76)
        signature = bytes(image.img[fn:fn + 35])
        checks = {0x194: b"\x44\x8b\x05", 0x78F: bytes.fromhex("488dbef82b0000"), 0x867: bytes.fromhex("4869c848060000")}
        calls = (0x705, 0x721, 0x876, 0x89D, 0xA76)
        lo, hi = image.text_va, image.text_va + len(image.text)

        def bt_finds(img):
            raw = img.img
            if raw.find(signature, lo, hi) != fn or raw.find(signature, fn + 1, hi) != -1:
                return False
            return all(bytes(raw[fn + o:fn + o + len(b)]) == b for o, b in checks.items()) and \
                all(raw[fn + o] == 0xE8 and bytes(raw[fn + o:fn + o + 5]) == bytes(image.img[fn + o:fn + o + 5]) for o in calls)

        def patched(sites):
            raw = bytearray(image.img)
            for s in sites:
                raw[s:s + 5] = b"\xe9\x11\x11\x11\x11"
            out = type(image).__new__(type(image))
            out.__dict__.update(image.__dict__)
            out.img = bytes(raw)
            return out

        self.assertEqual(struct.unpack_from("<i", image.img, fn + 0xA77)[0] + fn + 0xA7B, 0x40DFE0, "+ 0xa76 calls ui/AmmoFinalizer")
        self.assertTrue(bt_finds(image), "Building Tools finds the selector on the game's own code")
        self.assertTrue(bt_finds(patched([body, done])), "and with Kiln's two halves in (5-byte jumps at both sites)")
        self.assertFalse(bt_finds(patched([fn])), "an entry hook (kiln-tools and kiln-items until 0.2.0-dev.31.13) hides it")
        print(f"client 26430625: selector halves at {body:#x} and {done:#x}; Building Tools' Stocks lookup keeps its bytes", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True)
    parser.add_argument("--server", required=True)
    MaterialLookupSignatureTest.args = parser.parse_args()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
