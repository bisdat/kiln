#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fake Kiln clients at the join check of authored NPC bodies (16f; docs/kiln/network.md "NPC bodies at the join"), against a lab server
with "labPort" in kiln/config/network.json that built npc-body-test's body (lab.sh drives it during the second start):

    handshake.py --port 31161 --lab ~/srv/enshrouded/labs/dev31-16f

The server's Ready bodies are read from its ledger (kiln/data/kiln-quests/native/npc-bodies.ledger.json). A client that built the same
bodies is welcomed and finds the same hash in WELCOME; one whose closure differs, one with no bodies (an older Kiln) and one whose body
waits for a restart are refused with the reason the player reads. Uses tools/nettest/nettest.py's protocol client. PASS/FAIL lines;
exits non-zero on any FAIL.
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools", "nettest"))
import nettest as nt  # noqa: E402

MODS = [("npc-body-test", "0.1.0", 2), ("npc-body-broken", "0.1.0", 2)]
fails = 0


def check(name, ok, detail=""):
    global fails
    fails += 0 if ok else 1
    print(f"{'PASS' if ok else 'FAIL'} handshake.{name}{': ' + detail if detail else ''}", flush=True)


def props(lines, pending=None):
    p = {"nettest.client": "npc-body-handshake", "kiln.npcbodies": hashlib.sha256("\n".join(lines).encode()).hexdigest() if lines else "",
         "kiln.npcbodies.count": str(len(lines))}
    if lines:
        p["kiln.npcbodies.list"] = ",".join(lines)
    if pending:
        p["kiln.npcbodies.pending"] = pending
    return p


def join(port, name, p):
    c = nt.Client(port, name)
    c.send(nt.KNOCK)
    c.hello(MODS, [], p)
    c.wait(lambda: c.welcome is not None or c.reject is not None, 10)
    c.s.close()  # as the other lab harnesses do: the server times the peer out
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--lab", required=True)
    a = ap.parse_args()
    ledger = json.loads((Path(a.lab).expanduser() / "kiln/data/kiln-quests/native/npc-bodies.ledger.json").read_text())
    lines = sorted(f"{b['mod']}:{b['npc']}:{b['closure']}" for b in ledger["bodies"] if b.get("state") == "ready")
    check("server-bodies", lines and lines[0].startswith("npc-body-test:figure:"), f"{lines}")

    same = join(a.port, "same-bodies", props(lines))
    check("same-bodies-welcomed", same.welcome is not None, f"reject {same.reject}")
    if same.welcome is not None:
        got = same.welcome["props"]
        check("welcome-carries-the-hash", got.get("kiln.npcbodies") == props(lines)["kiln.npcbodies"] and got.get("kiln.npcbodies.count") == str(len(lines)),
              f"{got.get('kiln.npcbodies')} count {got.get('kiln.npcbodies.count')}")

    other = [line.rsplit(":", 1)[0] + ":" + "0" * 64 for line in lines]
    c = join(a.port, "other-closure", props(other))
    reason = c.reject[0] if c.reject else ""
    check("other-closure-refused", c.welcome is None and "the NPC body npc-body-test:figure is built differently on your game" in reason, reason or "welcomed")

    c = join(a.port, "no-bodies", props([]))
    reason = c.reject[0] if c.reject else ""
    check("no-bodies-refused", c.welcome is None and "the server has the NPC body npc-body-test:figure and your game doesn't" in reason, reason or "welcomed")

    c = join(a.port, "pending", props(lines, pending="npc-body-test:late"))
    reason = c.reject[0] if c.reject else ""
    check("pending-restart-refused", c.welcome is None and "restart required" in reason and "npc-body-test:late" in reason, reason or "welcomed")
    print(f"handshake: {5 + (same.welcome is not None)} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
