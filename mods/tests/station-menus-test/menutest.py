#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fake clients for station menus with tabs of their own (chapter 16a; docs/kiln/sync.md "Station menus").

Needs a lab server with "labPort" in kiln/config/network.json and the mod station-menus-test loaded:

    mods/tests/station-menus-test/menutest.py --port 31250 --lab ~/srv/enshrouded/labs/dev31-16a [--verbose]

Speaks kiln:station-menu v1 like a Kiln client (parts, SHA-256 state id, envelope, acks Stored and Applied), drives
station-menus-test:ctl, and checks admission: a Kiln client without the channel (or with version 0) is refused before
it enters the world with "station-menu protocol1 required", a client without Kiln is not refused for the menus. Uses
the protocol client of tools/nettest/nettest.py. Prints PASS/FAIL lines; exits non-zero on any FAIL.
"""
import argparse
import hashlib
import json
import os
import re
import sys
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools", "nettest"))
import nettest as nt  # noqa: E402

MENU, CTL, CONTENT = "kiln:station-menu", "station-menus-test:ctl", "kiln:content"
OURS = {MENU: "1", CTL: "1", CONTENT: "1"}
REFUSAL = "station-menu protocol1 required"
STORED, APPLIED, REFUSED = 0, 1, 2


class MenuClient(nt.Client):
    def __init__(self, port, name, verbose=False, channels=None):
        super().__init__(port, name, verbose)
        self.declared = channels if channels is not None else [(MENU, "1", False), (CTL, "1", False), (CONTENT, "1", False)]
        self.parts = {}
        self.snapshots = []   # dict(state, session, generation, raw, doc, parts)
        self.contents = []    # kiln:content state ids (u32) seen complete
        self.replies = []
        self.bad = []

    def set_table(self, table):
        self.table = table
        self.channels = {n: i for i, n, v, _ in table if OURS.get(n) == v}

    def connect(self):
        self.send(nt.KNOCK)
        self.hello([("station-menus-test", "0.1.0", 2)], self.declared)
        return self.wait(lambda: self.welcome is not None or self.reject is not None, 8)

    def pump(self, seconds):
        super().pump(seconds)
        keep = []
        for cid, mid, body in self.messages:
            if cid == self.channels.get(MENU) and mid == 0:
                self._part(body)
            elif cid == self.channels.get(CTL) and mid == 1:
                self.replies.append(nt.R(body).string())
            elif cid == self.channels.get(CONTENT) and mid == 0:
                r = nt.R(body)
                r.u16()
                sid = r.u32()
                if sid not in self.contents:
                    self.contents.append(sid)
            else:
                keep.append((cid, mid, body))
        self.messages = keep

    def _part(self, body):
        r = nt.R(body)
        fmt, sid, session, gen, raw_len, part, parts, codec = r.u16(), r.string(), r.string(), r.u64(), r.u32(), r.u16(), r.u16(), r.u8()
        data = r.blob()
        key = (sid, session, gen)
        got = self.parts.setdefault(key, {})
        got[part] = data
        if len(got) < parts:
            return
        del self.parts[key]
        packed = b"".join(got[i] for i in range(parts))
        raw = zlib.decompressobj(-15).decompress(packed)
        if fmt != 1 or codec != 1 or len(raw) != raw_len or hashlib.sha256(raw).hexdigest() != sid:
            self.bad.append(f"format {fmt} codec {codec} len {len(raw)}/{raw_len} hash ok {hashlib.sha256(raw).hexdigest() == sid}")
            return
        self.snapshots.append(dict(state=sid, session=session, generation=gen, raw=raw, doc=json.loads(raw), parts=parts))

    def ack(self, snap, stage, reason=""):
        w = nt.W().string(snap["state"]).string(snap["session"]).u64(snap["generation"]).u8(stage).string(reason)
        self.data(MENU, 1, w.b)

    def ctl(self, cmd, seconds=5.0):
        self.replies.clear()
        self.data(CTL, 0, nt.W().string(cmd).b)
        self.wait(lambda: len(self.replies) > 0, seconds)
        return self.replies[0] if self.replies else None

    def next_snapshot(self, count, seconds=10.0):
        self.wait(lambda: len(self.snapshots) >= count, seconds)
        return self.snapshots[count - 1] if len(self.snapshots) >= count else None


class Suite:
    def __init__(self, port, lab, verbose):
        self.port, self.lab, self.verbose = port, lab, verbose
        self.fails = 0

    def check(self, name, ok, detail=""):
        print(f"{'PASS' if ok else 'FAIL'} {name}{'' if ok or not detail else ': ' + str(detail)[:600]}", flush=True)
        if not ok:
            self.fails += 1

    def kiln_log(self):
        return os.path.join(self.lab, "kiln", "logs", "kiln.log")

    def mark(self):
        return os.path.getsize(self.kiln_log()) if os.path.exists(self.kiln_log()) else 0

    def since(self, m):
        with open(self.kiln_log(), "rb") as f:
            f.seek(m)
            return f.read().decode("utf-8", "replace")

    def wait_log(self, m, pattern, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if re.search(pattern, self.since(m)):
                return True
            time.sleep(0.1)
        return re.search(pattern, self.since(m)) is not None

    def peer_of(self, m):
        ids = re.findall(r"net: lab peer (0x[0-9a-f]+) up", self.since(m))
        return ids[-1] if ids else None

    def run(self):
        # ---- a Kiln client with the channel: Welcome, the initial snapshot on join, acks
        m = self.mark()
        a = MenuClient(self.port, "menu-a", self.verbose)
        self.check("station-menu.welcome", a.connect() and a.welcome is not None, a.reject)
        a.send(nt.LAB_READY)
        s1 = a.next_snapshot(1)
        self.check("station-menu.snapshot-on-join", s1 is not None, a.bad)
        if s1 is None:
            return
        doc = s1["doc"]
        self.check("station-menu.envelope", sorted(doc) == ["contentStateId", "format", "generation", "menus", "session"] and doc["format"] == 1 and
                   doc["session"] == s1["session"] and doc["generation"] == s1["generation"], list(doc))
        self.check("station-menu.canonical-bytes", s1["raw"] == json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode(), "")
        menus = {x["origin"]["stationKey"]: x for x in doc["menus"]}
        forge = menus.get("forge")
        self.check("station-menu.forge-menu", forge is not None and [t["key"] for t in forge["tabs"]] == ["metal", "fine"], list(menus))
        if forge:
            first = forge["tabs"][0]["sets"][0]
            self.check("station-menu.forge-recipes", [r["key"] for r in first["recipes"]] == ["station-menus-test:reclaim", "station-menus-test:bolts"] and
                       first["heading"] is None, first)
            self.check("station-menu.texts", forge["tabs"][0]["name"] == {"de": "Metall", "en": "Metal"} and forge["tabs"][1]["sets"][1]["heading"] == {"*": "Fine work"},
                       forge["tabs"])
        bench = menus.get("empty-bench")
        self.check("station-menu.empty-first-join", bench is not None and all(not s["recipes"] for t in bench["tabs"] for s in t["sets"]), bench)
        a.wait(lambda: len(a.contents) > 0, 8)
        self.check("station-menu.content-state", doc["contentStateId"] in a.contents or doc["contentStateId"] == 0,
                   f"{doc['contentStateId']:x} vs {[hex(c) for c in a.contents]}")
        a.ack(s1, STORED)
        a.ack(s1, APPLIED)
        self.check("station-menu.acks-logged", self.wait_log(m, r"\): snapshot " + s1["state"][:12] + r" generation \d+ applied", 5), self.since(m)[-500:])

        # ---- a layer of code over the inline one, then gone again: new snapshots; the same define again sends nothing
        g0 = s1["generation"]
        self.check("station-menu.define-winter", a.ctl("define winter") == "ok", a.replies)
        s2 = a.next_snapshot(2)
        winter = s2 and {x["origin"]["stationKey"]: x for x in s2["doc"]["menus"]}.get("forge")
        self.check("station-menu.replacement", s2 is not None and s2["generation"] > g0 and winter is not None and winter["definition"]["key"] == "forge.winter",
                   s2 and s2["doc"])
        self.check("station-menu.replay-idempotent", a.ctl("define winter") == "ok" and a.next_snapshot(3, 4) is None, len(a.snapshots))
        self.check("station-menu.remove-winter", a.ctl("remove winter") == "ok", a.replies)
        s3 = a.next_snapshot(3)
        back = s3 and {x["origin"]["stationKey"]: x for x in s3["doc"]["menus"]}.get("forge")
        self.check("station-menu.owner-reveal", back is not None and back["definition"]["key"] == "forge.menu", s3 and s3["doc"])

        # ---- an older Kiln client (no channel) and version 0: refused before the world, with the chapter's text
        for name, chans in (("menu-old", [(CTL, "1", False)]), ("menu-v0", [(MENU, "0", False), (CTL, "1", False)])):
            m2 = self.mark()
            o = MenuClient(self.port, name, self.verbose, chans)
            o.connect()
            pid = None
            for _ in range(30):
                pid = self.peer_of(m2)
                if pid:
                    break
                time.sleep(0.1)
            o.send(nt.LAB_READY)
            o.wait(lambda: o.reject is not None, 6)
            self.check(f"station-menu.{name}-refused", o.reject is not None and o.reject[0] == REFUSAL, o.reject)
            time.sleep(1.0)
            self.check(f"station-menu.{name}-never-in-world", pid is not None and f"lab peer {pid} passed the gate" not in self.since(m2), pid)

        # ---- a client without Kiln: not refused for the menus (today's rules decide it)
        m3 = self.mark()
        v = nt.Client(self.port, "menu-vanilla", self.verbose)
        v.send(nt.LAB_VANILLA)
        time.sleep(0.5)
        v.send(nt.LAB_READY)
        time.sleep(4.0)
        log = self.since(m3)
        self.check("station-menu.vanilla-not-refused-for-menus", "for kiln.stationmenus" not in log and REFUSAL not in log, log[-400:])

        # ---- every menu taken off: the tombstone (menus: []), and the admission decider goes
        n = len(a.snapshots)  # before the command: the snapshot can arrive while its reply is awaited
        self.check("station-menu.remove-all", a.ctl("remove all") == "ok", a.replies)
        s4 = a.next_snapshot(n + 1)
        self.check("station-menu.tombstone", s4 is not None and s4["doc"]["menus"] == [], s4 and s4["doc"])
        m4 = self.mark()
        o = MenuClient(self.port, "menu-old-after", self.verbose, [(CTL, "1", False)])
        o.connect()
        o.send(nt.LAB_READY)
        o.wait(lambda: o.reject is not None, 4)
        self.check("station-menu.no-menu-no-refusal", o.reject is None and o.welcome is not None, o.reject)
        a.ctl("restore")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=31250)
    ap.add_argument("--lab", required=True)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    s = Suite(a.port, os.path.expanduser(a.lab), a.verbose)
    s.run()
    print(f"menutest: {s.fails} failure(s)", flush=True)
    sys.exit(1 if s.fails else 0)


if __name__ == "__main__":
    main()
