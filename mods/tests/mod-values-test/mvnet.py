#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Kiln.ModValues without a client: fake Kiln peers on kiln.net's lab transport (tools/nettest).

A lab peer has no player: it joins as a fixed character through kiln:character's lab join (CharLabJoin), then reads the
stream kiln:mod-values as a Kiln client would (parts, digest, ack) and asks the mod mod-values-test over
"mod-values-test:ctl" to act for it. Needs "labPort" in kiln/config/network.json.

    mvnet.py --port P --phase first|restart

Peers: A (character CA; kiln:character, kiln:mod-values, ctl), B (character CB; kiln:character and ctl, no stats channel:
a client without the channel), O (kiln:mod-values and ctl, never joins: another peer), V (no channel at all: vanilla).
"""
import argparse
import base64
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools", "nettest"))
import nettest as nt  # noqa: E402

CH, MV, CTL = "kiln:character", "kiln:mod-values", "mod-values-test:ctl"
LABJOIN, LABLEAVE = 5, 6                                    # CharacterSync.cs registration order
HELLO, PART, ACK, RESYNC, OFFER, RESULT = range(6)          # chapter 05b section 4.4, Sync/Stream.cs
REQ, REPLY = range(2)
ACCOUNT = 0x1111222233336666
CA = "aaaaaaaa-0000-4000-8000-0000000005b1"
CB = "bbbbbbbb-0000-4000-8000-0000000005b2"


class Peer(nt.Client):
    want = ()

    def set_table(self, table):
        self.table = table
        self.channels = {n: i for i, n, v, _ in table if n in self.want and v == "1"}

    def js(self, ch, msg, obj):
        self.data(ch, msg, json.dumps(obj).encode())

    def get(self, ch, msg, seconds=8.0):
        b = self.take(ch, msg, seconds)
        return None if b is None else json.loads(b.rstrip(b"\0").decode())

    def ctl(self, rid, op, character=CA, amount=0):
        self.js(CTL, REQ, {"Id": rid, "Op": op, "Character": character, "Amount": amount})
        return self.get(CTL, REPLY, 15)

    def join(self, character):
        self.js(CH, LABJOIN, {"Character": character, "Account": ACCOUNT})
        self.pump(0.5)

    def leave(self, character):
        self.js(CH, LABLEAVE, {"Character": character})
        self.pump(0.5)

    def stats_frames(self):
        cid = self.channels.get(MV)
        return [m for m in self.messages if cid is not None and m[0] == cid]


class Stream:
    """The client's reducer, minimal: one full or delta at a time, digest checked, then ack."""

    def __init__(self, peer):
        self.p, self.nonce, self.seq, self.values, self.defs = peer, None, 0, {}, {}

    def hello(self, seconds=10):
        h = self.p.get(MV, HELLO, seconds)
        if h:
            self.nonce, self.seq, self.values, self.defs = h["nonce"], 0, {}, {}
        return h

    def transfer(self, seconds=15):
        parts, first = {}, None
        for _ in range(int(seconds / 0.5)):
            m = self.p.get(MV, PART, 0.5)
            if m is None:
                continue
            if m["nonce"] != self.nonce:
                continue
            first = first or m
            if m["seq"] != first["seq"]:
                parts, first = {}, m
            parts[m["part"]] = base64.b64decode(m["payloadB64"])
            if len(parts) == m["parts"]:
                doc = b"".join(parts[i] for i in range(m["parts"]))
                if len(doc) != m["totalBytes"] or hashlib.sha256(doc).hexdigest() != m["sha256"]:
                    return None, "digest"
                p = json.loads(doc)
                if m["full"]:
                    self.values, self.defs = {}, {}
                for k in p.get("removed", []):
                    self.values.pop(k, None)
                    self.defs.pop(k, None)
                for d in p.get("definitions", []):
                    self.defs[d["key"]] = d
                for v in p.get("values", []):
                    self.values[v["key"]] = v
                self.seq = int(m["seq"])
                self.p.js(MV, ACK, {"nonce": self.nonce, "seq": m["seq"], "transfer": m["transfer"]})
                return m, p
        return None, "timeout"

    def raw(self, key):
        v = self.values.get(key)
        return None if v is None else int(v["raw"])

    def offer(self, entries, transfer="1"):
        doc = json.dumps({"schema": 1, "sidecarRevision": "0", "entries": entries}, separators=(",", ":")).encode()
        self.p.js(MV, OFFER, {"nonce": self.nonce, "transfer": transfer, "part": 0, "parts": 1, "totalBytes": len(doc),
                              "sha256": hashlib.sha256(doc).hexdigest(), "payloadB64": base64.b64encode(doc).decode()})
        return self.p.get(MV, RESULT, 10)


class Suite:
    def __init__(self):
        self.passed = self.failed = 0

    def check(self, name, ok, detail=""):
        print(f"{'PASS' if ok else 'FAIL'} mod-values: {name}{'  ' + str(detail)[:300] if detail != '' else ''}", flush=True)
        if ok:
            self.passed += 1
        else:
            self.failed += 1


def peer(port, name, want):
    p = Peer(port, name)
    p.want = want
    p.hello([], [(c, "1", False) for c in want])
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--phase", choices=["first", "restart"], required=True)
    a = ap.parse_args()
    t = Suite()
    A = peer(a.port, "mv-A", (CH, MV, CTL))
    B = peer(a.port, "mv-B", (CH, CTL))
    O = peer(a.port, "mv-O", (MV, CTL))
    V = peer(a.port, "mv-V", ())
    for p in (A, B, O, V):
        t.check(f"handshake {p.name}", p.wait(lambda p=p: p.welcome is not None, 20))
    if t.failed:
        sys.exit(1)
    sa = Stream(A)
    A.join(CA)
    B.join(CB)
    h = sa.hello(20)
    t.check("owner hello", h is not None and h["status"] == "ready" and h["character"].endswith("." + CA.replace("-", "")), h)
    m, p = sa.transfer()
    t.check("owner stream full seq=1", m is not None and m["full"] and m["seq"] == "1" and "mod-values-test:coins" in sa.defs, (m and m["seq"], p if m is None else ""))
    if a.phase == "first":
        r = A.ctl(1, "read")
        t.check("new character starts at the initials", r and r["Xp"] == 0 and r["Coins"] == 0 and r["Tokens"] == 10 and r["Tick"], r)
        t.check("json value defined (tokens initial 10)", r and r["Tokens"] == 10, r)
        r = A.ctl(2, "award")
        t.check("command tick=True source=Player", r and r["Ok"] and r["Tick"] and r["Source"] == "Player", r)
        t.check("award xp=7 coins=11 rev=1", r and r["Xp"] == 7 and r["Coins"] == 11 and r["Rev"] == 1, r)
        m, p = sa.transfer()
        t.check("owner stream delta seq=2 coins=11", m is not None and not m["full"] and m["seq"] == "2" and sa.raw("mod-values-test:coins") == 11, (m and m["seq"], sa.values.get("mod-values-test:coins")))
        same = A.ctl(2, "award")
        t.check("duplicate request not executed twice", same == r, same)
        bad = A.ctl(3, "spend", amount=20)
        t.check("insufficient funds refused", bad and not bad["Ok"] and bad["Reason"] == "InsufficientFunds" and bad["Coins"] == 11, bad)
        good = A.ctl(4, "spend", amount=3)
        t.check("spend coins=8 rev=2", good and good["Ok"] and good["Coins"] == 8 and good["Rev"] == 2, good)
        denied = A.ctl(5, "admin")
        t.check("admin command refused", denied and not denied["Ok"] and "may not" in denied["Reason"], denied)
        s = B.ctl(6, "read", CB)
        t.check("second character separate", s and s["Xp"] == 0 and s["Coins"] == 0, s)
        seeded = A.ctl(7, "carry-seed", amount=300)
        t.check("carried seeded 300", seeded and seeded["Carried"] == 300, seeded)
        sa.transfer(5)
        res = sa.offer([
            {"key": "mod-values-test:carried", "raw": "500", "kind": 2, "scale": 0, "definitionVersion": 1, "carryEpoch": 1},
            {"key": "mod-values-test:coins", "raw": "999999", "kind": 1, "scale": 0, "definitionVersion": 1, "carryEpoch": 1},
        ])
        after = A.ctl(8, "read")
        t.check("carry offer max-merged", res and res["ok"] and res["applied"] == 1 and res["rejected"] == 1 and after["Carried"] == 500 and after["Coins"] == 8, (res, after))
        again = sa.offer([{"key": "mod-values-test:carried", "raw": "900", "kind": 2, "scale": 0, "definitionVersion": 1, "carryEpoch": 1}], transfer="2")
        after2 = A.ctl(9, "read")
        t.check("second offer in one stream refused", again and not again["ok"] and after2["Carried"] == 500, (again, after2))
        m, p = sa.transfer()
        t.check("applied offer forces a full", m is not None and m["full"] and sa.raw("mod-values-test:carried") == 500, m and m["seq"])
        O.pump(2.0)
        B.pump(0.5)
        V.pump(0.5)
        t.check("other peer no frames", len(O.stats_frames()) == 0, len(O.stats_frames()))
        known = {B.channels.get(CH), B.channels.get(CTL)}
        t.check("peer without the channel no frames", all(m[0] in known for m in B.messages), [m[0] for m in B.messages])
        t.check("vanilla peer no frames", len(V.messages) == 0, len(V.messages))
        # character reload: leave and join again: a new nonce, a new full, the same values
        old = sa.nonce
        A.leave(CA)
        A.join(CA)
        h = sa.hello(20)
        m, p = sa.transfer()
        t.check("character reload: new nonce, full seq=1, coins=8", h and sa.nonce != old and m and m["seq"] == "1" and sa.raw("mod-values-test:coins") == 8, (h and h["nonce"], m and m["seq"], sa.values.get("mod-values-test:coins")))
        saved = A.ctl(10, "save")
        t.check("world saved", saved and saved["Ok"], saved)
    else:
        r = A.ctl(1, "read")
        t.check("restart xp=7 coins=8 rev=2", r and r["Xp"] == 7 and r["Coins"] == 8 and r["Rev"] == 2, r)
        t.check("restart carried=500 tokens=10", r and r["Carried"] == 500 and r["Tokens"] == 10, r)
        t.check("restart stream coins=8", sa.raw("mod-values-test:coins") == 8 and sa.raw("mod-values-test:xp") == 7, sa.values)
        s = B.ctl(2, "read", CB)
        t.check("restart second character still separate", s and s["Xp"] == 0 and s["Coins"] == 0, s)
        O.pump(1.0)
        t.check("other peer no frames", len(O.stats_frames()) == 0, len(O.stats_frames()))
    print(f"mod-values: {t.passed} PASS, {t.failed} FAIL", flush=True)
    sys.exit(1 if t.failed else 0)


if __name__ == "__main__":
    main()
