#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Joined-server UI commands without a client: fake Kiln peers on kiln.net's lab transport (tools/nettest).

Speaks kiln:ui-command v1 (dossier 09 section 4.5; Kiln.Blocks Ui/UiCommandWire.cs) as a Kiln client's
BlockUi.ExecuteOnServer does, against the ui-blocks-test mod's test commands (UiCommandLab.cs). A lab peer has no player:
over "ui-blocks-test:ctl" it asks the mod to stand a synthetic entity in for it (the game's own record for a real player).
Needs "labPort" in kiln/config/network.json.

    uicmdnet.py --port P

Peers: A (entity 900: the requests), B (entity 901: its player goes away with work queued), N (the channel, never bound:
no player), V (no kiln:ui-command at all).
"""
import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools", "nettest"))
import nettest as nt  # noqa: E402

UI, CTL = "kiln:ui-command", "ui-blocks-test:ctl"
HELLO, REQUEST, REPLY, CANCEL, STATUS = range(5)   # registration order (UiCommandWire)
CTLREQ, CTLREPLY = range(2)


class Peer(nt.Client):
    want = ()

    def __init__(self, port, name):
        super().__init__(port, name)
        self.replies, self.hellos, self.ui_frames, self.ctl_id = [], [], 0, 0

    def set_table(self, table):
        self.table = table
        self.channels = {n: i for i, n, v, _ in table if n in self.want and v == "1"}
        self.ui_server_id = next((i for i, n, v, _ in table if n == UI), None)

    def pump(self, seconds):
        super().pump(seconds)
        ui, keep = self.channels.get(UI), []
        for m in self.messages:
            if self.ui_server_id is not None and m[0] == self.ui_server_id:
                self.ui_frames += 1
            if ui is not None and m[0] == ui and m[1] == REPLY:
                self.replies.append(json.loads(m[2].rstrip(b"\0")))
            elif ui is not None and m[0] == ui and m[1] == HELLO:
                self.hellos.append(json.loads(m[2].rstrip(b"\0"))["nonce"])
            else:
                keep.append(m)
        self.messages = keep

    def js(self, ch, msg, obj):
        self.data(ch, msg, json.dumps(obj).encode())

    def ctl(self, op, entity=0, tag=""):
        self.ctl_id += 1
        self.js(CTL, CTLREQ, {"Id": self.ctl_id, "Op": op, "Entity": entity, "Tag": tag})
        for _ in range(200):
            b = self.take(CTL, CTLREPLY, 0.05)
            if b is not None:
                r = json.loads(b.rstrip(b"\0"))
                if r["Id"] == self.ctl_id:
                    return r
        return None

    def count(self, tag):
        r = self.ctl("count", tag=tag)
        return None if r is None else r["Count"]

    # kiln:ui-command
    def send(self, msg, obj):
        if isinstance(obj, dict) and "id" in obj:
            self.replies = [r for r in self.replies if r["id"] != obj["id"]]
        if isinstance(obj, (bytes, bytearray)):
            self.data(UI, msg, obj)
        else:
            self.js(UI, msg, obj)

    def request(self, nonce, rid, line):
        self.send(REQUEST, {"nonce": nonce, "id": str(rid), "line": line})

    def reply(self, rid, final=True, seconds=10.0, state=None):
        end = time.time() + seconds
        while time.time() < end:
            for r in self.replies:
                if r["id"] == str(rid) and (not final or r["state"] == "final") and (state is None or r["state"] == state):
                    return r
            self.pump(0.05)
        return None

    def run(self, nonce, rid, line, seconds=10.0):
        self.request(nonce, rid, line)
        return self.reply(rid, seconds=seconds)


class Suite:
    def __init__(self, ctl):
        self.passed = self.failed = 0
        self.ctl = ctl

    def check(self, name, ok, detail=""):
        line = f"{'PASS' if ok else 'FAIL'} ui-command: {name}{'  ' + str(detail)[:300] if detail != '' else ''}"
        print(line, flush=True)
        if ok:
            self.passed += 1
        else:
            self.failed += 1
        if self.ctl is not None:
            self.ctl.ctl("check", 1 if ok else 0, f"ui-command (fake client): {name}")


def peer(port, name, want):
    p = Peer(port, name)
    p.want = want
    p.hello([], [(c, "1", False) for c in want])
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    a = ap.parse_args()
    A = peer(a.port, "ui-A", (UI, CTL))
    B = peer(a.port, "ui-B", (UI, CTL))
    N = peer(a.port, "ui-N", (UI, CTL))
    V = peer(a.port, "ui-V", (CTL,))
    t = Suite(None)
    for p in (A, B, N, V):
        t.check(f"handshake {p.name}", p.wait(lambda p=p: p.welcome is not None, 30))
    if t.failed:
        sys.exit(1)
    t.ctl = A
    t.check("the server has kiln:ui-command", A.ui_server_id is not None and UI in A.channels, A.table)

    # N: the channel, no player: no hello, a guessed nonce runs nothing and gets no answer
    N.pump(4)
    N.request("0" * 32, 1, "uicmd-echo n1")
    N.pump(2)
    no_player = (len(N.hellos), len(N.replies))

    A.ctl("bind", 900)
    t.check("hello after the player is there", A.wait(lambda: len(A.hellos) > 0, 15) and re.fullmatch("[0-9a-f]{32}", A.hellos[-1]) is not None, A.hellos)
    n = A.hellos[-1]

    r = A.run(n, 1, "uicmd-echo t1")
    t.check("source=Player line ok", r is not None and r["ok"] and r["code"] == "none" and r["output"] == ["source=Player player=900 tag=t1"], r)
    time.sleep(0.3)
    r = A.run(n, 2, "uicmd-admin t2")
    t.check("nonadmin refused", r is not None and not r["ok"] and r["code"] == "permissionDenied" and (r["reason"] or "").startswith("You may not use /uicmd-admin") and A.count("t2") == 0, r)
    time.sleep(0.3)
    r = A.run(n, 3, "uicmd-cancelme t3")
    t.check("canceled by command event", r is not None and not r["ok"] and r["code"] == "canceledByCommandEvent" and "stopped by ui-blocks-test" in (r["reason"] or "") and A.count("t3") == 0, r)
    time.sleep(0.3)
    raw = json.dumps({"nonce": n, "id": "4", "line": "uicmd-echo t4", "player": "1", "entity": 1, "account": "76561198000000001",
                      "admin": True, "mod": "ui-blocks-test", "source": "console", "steamId": "76561198000000001"}).encode()
    A.send(REQUEST, raw)
    r = A.reply(4)
    t.check("claimed identity ignored", r is not None and r["ok"] and r["output"] == ["source=Player player=900 tag=t4"], r)
    time.sleep(0.3)
    r = A.run(n, 1, "uicmd-echo t1")
    t.check("duplicate id returns cached reply", r is not None and r["ok"] and r["output"] == ["source=Player player=900 tag=t1"] and A.count("t1") == 1, (r, A.count("t1")))
    time.sleep(0.3)
    r = A.run(n, 1, "uicmd-echo t5")
    t.check("conflict never executes", r is not None and r["code"] == "protocolConflict" and not r["ok"] and A.count("t5") == 0, r)
    A.send(STATUS, {"nonce": n, "id": "1"})
    s1 = A.reply(1)
    A.send(STATUS, {"nonce": n, "id": "99"})
    s99 = A.reply(99)
    A.send(CANCEL, {"nonce": n, "id": "98"})
    c98 = A.reply(98)
    t.check("status request answers and runs nothing", s1 is not None and s1["ok"] and s99 is not None and s99["code"] == "unknownRequest" and A.count("t1") == 1, (s1, s99))
    t.check("cancel of an unknown id is UnknownRequest", c98 is not None and c98["code"] == "unknownRequest", c98)
    r = A.run("f" * 32, 50, "uicmd-echo t6")
    t.check("another nonce never runs", r is not None and r["code"] == "associationChanged" and A.count("t6") == 0, r)

    # 4 a second, 8 at once: ten at once, two refused and never run
    time.sleep(3)
    for i in range(5, 15):
        A.request(n, i, f"uicmd-echo r{i}")
    finals = [A.reply(i) for i in range(5, 15)]
    limited = [f for f in finals if f and f["code"] == "rateLimited"]
    ran = [f for f in finals if f and f["ok"]]
    counts = {f["id"]: A.count("r" + f["id"]) for f in limited}
    t.check("rate limited", len(limited) == 2 and len(ran) == 8 and all(c == 0 for c in counts.values()), ([f and f["code"] for f in finals], counts))

    # an async command: started first, then its final with what it said after it began
    time.sleep(3)
    A.request(n, 15, "uicmd-slow s1 75")
    started = A.reply(15, final=False, state="started", seconds=5)
    r = A.reply(15, seconds=30)
    t.check("async final output", started is not None and r is not None and r["ok"] and r["output"] == ["slow done s1"], (started, r))

    # two running hold this connection's slots: a third waits 5 s and expires, never run
    A.request(n, 16, "uicmd-slow s2 300")
    A.request(n, 17, "uicmd-slow s3 300")
    A.reply(17, final=False, state="started", seconds=5)
    A.request(n, 18, "uicmd-echo q1")
    q = A.reply(18, final=False, state="queued", seconds=5)
    A.request(n, 19, "uicmd-echo q2")
    A.reply(19, final=False, state="queued", seconds=5)
    A.send(CANCEL, {"nonce": n, "id": "19"})
    c19 = A.reply(19, seconds=5)
    A.send(CANCEL, {"nonce": n, "id": "16"})
    c16 = A.reply(16, final=False, seconds=5)
    r = A.reply(18, seconds=10)
    t.check("queue expired not started", q is not None and r is not None and r["code"] == "expiredBeforeStart" and A.count("q1") == 0, (q, r))
    t.check("queued cancel not started", c19 is not None and c19["code"] == "canceledBeforeStart" and A.count("q2") == 0, c19)
    t.check("cancel after the start is too late", c16 is not None and c16["code"] == "tooLateToCancel" and c16["state"] in ("started", "runningOverBudget"), c16)
    r16, r17 = A.reply(16, seconds=40), A.reply(17, seconds=40)
    t.check("the started ones end on their own", r16 is not None and r16["ok"] and r17 is not None and r17["ok"], (r16, r17))

    # B: its player goes away with two running and one queued: the queued one ends unrun, the running ones keep their
    # slots and answer nobody (the lab transport's 60 s idle is longer than the 5 s queue deadline, so a lab disconnect
    # cannot show this; the player leaving the connection is the same retirement)
    B.ctl("bind", 901)
    B.wait(lambda: len(B.hellos) > 0, 15)
    nb = B.hellos[-1] if B.hellos else ""
    B.request(nb, 1, "uicmd-slow b1 150")
    B.request(nb, 2, "uicmd-slow b2 150")
    B.reply(2, final=False, state="started", seconds=5)
    B.request(nb, 3, "uicmd-echo b3")
    B.reply(3, final=False, state="queued", seconds=5)
    B.ctl("unbind")
    r3 = B.reply(3, seconds=5)
    B.pump(9)
    late = [x for x in B.replies if x["id"] in ("1", "2") and x["state"] == "final"]
    t.check("disconnect drops queued (the connection's player went away)", r3 is not None and r3["code"] == "associationChanged" and B.count("b3") == 0 and not late and B.count("b1") == 1,
            (r3, late, B.count("b3")))

    V.pump(1)
    t.check("vanilla peer no frames (a peer without kiln:ui-command)", V.ui_frames == 0 and not V.hellos and not V.replies, V.ui_frames)
    N.pump(1)
    t.check("no-player peer refused", no_player == (0, 0) and not N.hellos and not N.replies and A.count("n1") == 0, (no_player, len(N.hellos), len(N.replies)))
    print(f"ui-command: {t.passed} PASS, {t.failed} FAIL", flush=True)
    sys.exit(1 if t.failed else 0)


if __name__ == "__main__":
    main()
