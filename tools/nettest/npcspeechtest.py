#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Kiln.Npcs in a lab (docs/kiln/services/npc-speech.md): Kiln peers over kiln.net's lab transport that have the channel
kiln:npcspeech v1, a vanilla peer, and mods/tests/npcspeech-test driven through the console control folder.

    tools/nettest/npcspeechtest.py --port <labPort> --lab ~/srv/enshrouded/labs/<lab>

Checks what the host sends (Speech, Say, Reset: NpcSpeechEngine's wire v1), the snapshot a peer gets when it becomes
ready, that the same call again sends nothing, and that the vanilla peer gets no Kiln frame. Prints PASS/FAIL lines and
exits non-zero on any FAIL.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nettest  # noqa: E402

CHANNEL = ("kiln:npcspeech", "1")
SPEECH, SAY, RESET = 1, 2, 3
FAILS = []


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}", flush=True)
    if not ok:
        FAILS.append(name)


class Peer(nettest.Client):
    def set_table(self, table):
        self.table = table
        self.channels = {n: i for i, n, v, _ in table if (n, v) == CHANNEL}

    def speech_messages(self):
        cid = self.channels.get(CHANNEL[0])
        out = []
        for m in list(self.messages):
            if m[0] != cid:
                continue
            r = nettest.R(m[2])
            if m[1] == SPEECH:
                npc, mode, n = r.u32(), r.u8(), r.u8()
                out.append(("speech", npc, mode, [json.loads(r.string()) for _ in range(n)]))
            elif m[1] == SAY:
                npc, text, ms = r.u32(), r.string(), r.u32()
                out.append(("say", npc, json.loads(text) if text else None, ms))
            elif m[1] == RESET:
                out.append(("reset", r.boolean()))
        return out

    def drain(self, seconds=1.5):
        self.pump(seconds)
        got = self.speech_messages()
        self.messages.clear()
        return got


def console(lab, line, seconds=15):
    d = os.path.join(lab, "kiln", "control", "console")
    os.makedirs(d, exist_ok=True)
    name = f"npcspeech{int(time.time() * 1000) % 10**9}"
    result = os.path.join(d, name + ".result.json")
    with open(os.path.join(d, name + ".cmd.tmp"), "w") as f:
        f.write(line)
    os.replace(os.path.join(d, name + ".cmd.tmp"), os.path.join(d, name + ".cmd"))
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(result):
            time.sleep(0.2)
            with open(result) as f:
                doc = json.load(f)
            out = []
            for e in doc.get("results", []):
                out += [str(x) for x in e.get("output") or []] + ([e["error"]] if e.get("error") else [])
            return " | ".join(out) or json.dumps(doc)
        time.sleep(0.2)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--lab", required=True)
    a = ap.parse_args()
    npc = 900001

    k = Peer(a.port, "speech-kiln")
    k.hello([], [(CHANNEL[0], CHANNEL[1], False)])
    check("handshake of a Kiln peer with kiln:npcspeech v1", k.wait(lambda: k.welcome is not None, 20), repr(k.reject))
    check("the server has the channel, not required", any(n == CHANNEL[0] and v == CHANNEL[1] and not req for _, n, v, req in k.table),
          repr(k.table))
    first = k.drain(3)
    check("a ready peer gets the snapshot's Reset first", first[:1] == [("reset", True)], repr(first))

    v = nettest.Client(a.port, "speech-vanilla")
    v.send(nettest.LAB_VANILLA)
    time.sleep(0.3)
    v.send(nettest.LAB_READY)

    status = console(a.lab, "npcspeech-test status")
    print(f"INFO {status}", flush=True)
    check("the server runs the driver", status is not None and "npcspeech-test status" in status, repr(status))
    check("a dedicated server shows no bubbles itself (Npcs.Available false)", status is not None and "available False" in status, repr(status))

    r = console(a.lab, f"npcspeech-test say {npc}")
    got = k.drain()
    check("Say is accepted on the host", r is not None and ": ok;" in r and "saying True" in r, repr(r))
    say = [m for m in got if m[0] == "say"]
    check("the Kiln peer gets one Say", len(say) == 1, repr(got))
    if say:
        _, who, text, ms = say[0]
        check("Say: the NPC, both languages, 30 s", who == npc and text == {"de": "Ich verhungere...", "en": "I'm starving..."} and 29000 <= ms <= 30000,
              repr(say[0]))

    r = console(a.lab, f"npcspeech-test replace {npc}")
    got = k.drain()
    sp = [m for m in got if m[0] == "speech"]
    check("Replace is accepted", r is not None and "mode Replace" in r, repr(r))
    check("the Kiln peer gets Speech Replace with the three lines",
          len(sp) == 1 and sp[0][1] == npc and sp[0][2] == 2 and [x["*"] for x in sp[0][3]] == ["No food, no work.", "My stomach...", "Is there nothing to eat?"],
          repr(got))
    check("the running Say is not sent again", not [m for m in got if m[0] == "say"], repr(got))

    console(a.lab, f"npcspeech-test replace {npc}")
    check("the same Replace again sends nothing", k.drain() == [])

    late = Peer(a.port, "speech-late")
    late.hello([], [(CHANNEL[0], CHANNEL[1], False)])
    late.wait(lambda: late.welcome is not None, 20)
    snap = late.drain(3)
    kinds = [m[0] for m in snap]
    check("a peer that joins later gets Reset, the Replace and the running Say",
          kinds[:1] == ["reset"] and ("speech", npc, 2) in [(m[0], m[1], m[2]) for m in snap if m[0] == "speech"] and
          any(m[0] == "say" and m[1] == npc and m[3] > 0 for m in snap), repr(snap))

    r = console(a.lab, f"npcspeech-test suppress {npc}")
    got = k.drain()
    check("Suppress replaces this mod's Replace", r is not None and "mode Suppress" in r and
          [m[:3] for m in got if m[0] == "speech"] == [("speech", npc, 1)], repr(got))

    r = console(a.lab, f"npcspeech-test stop {npc}")
    got = k.drain()
    check("StopSaying sends an empty Say", r is not None and "saying False" in r and ("say", npc, None, 0) in got, repr(got))

    r = console(a.lab, f"npcspeech-test clear {npc}")
    got = k.drain()
    check("Clear sends the game's lines back (mode 0)", r is not None and "mode Game" in r and ("speech", npc, 0, []) in got, repr(got))
    late.drain(0.5)

    v.pump(1)
    check("the vanilla peer got no Kiln frame", not v.messages and v.welcome is None, repr(v.messages[:3]))
    print(f"npcspeech lab: {'FAIL' if FAILS else 'PASS'} ({len(FAILS)} failed)", flush=True)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
