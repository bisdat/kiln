#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Protocol-only Kiln peers against a dedicated disposable lab. Never starts a game client.
The native host suite covers export from real store layers; ContentJoinTests covers client application.
Run inside one wave lab session, after deploying RegistryMpTest and enabling labPort/modSync refuse.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools" / "nettest"))
import nettest as nt

p = argparse.ArgumentParser()
p.add_argument("--lab", type=Path, required=True)
p.add_argument("--port", type=int, required=True)
a = p.parse_args()
if not a.lab.name.startswith("dev31-registry-mp"):
    raise SystemExit("this fixture only drives disposable dev31-registry-mp labs")

clients = []
checks = 0
mods = [("registry-mp-test", "0.1.0", 2)]
channels = [("kiln:content", "1", False)]
key = "kiln.registry.boot"
wood = "0x2b0cf1d5"


def check(ok, what):
    global checks
    if not ok:
        raise AssertionError(what)
    checks += 1
    print("PASS " + what, flush=True)


def join(name, prop=None):
    c = nt.ContentClient(a.port, name)
    clients.append(c)
    c.hello(mods, channels, {key: prop} if prop else None)
    check(c.wait(lambda: c.welcome is not None or c.reject is not None, 15), name + " answered")
    return c


def state(c, value):
    def matches(s):
        d = s.get("doc")
        return d is not None and (d.get("items", {}).get(wood, {}).get("maxStackSize") == value if value else
                                  "maxStackSize" not in d.get("items", {}).get(wood, {}))
    check(c.wait(lambda: any(matches(s) for s in c.states), 30), "server registry layer received " + str(value))
    s = next(s for s in reversed(c.states) if matches(s))
    c.ack(s["id"])
    c.pump(0.2)


try:
    c = join("registry-old-peer")
    check(c.welcome is not None, "older peer without registry extension joins under refuse")
    fp = c.welcome["props"].get(key)
    check(fp is not None and len(fp) == 131 and fp.startswith("1:"), "WELCOME carries resolved registry values and order")
    initial = json.loads((a.lab / "kiln/state/registry.json").read_text())["bootSync"]
    check(fp == "1:" + initial["values"] + ":" + initial["order"], "native report reaches the real Host/Network seam")
    same = join("registry-matching-peer", fp)
    check(same.welcome is not None, "matching registry fingerprint joins")
    other = join("registry-values-mismatch", "1:" + "0" * 64 + fp[66:])
    check(other.reject is not None and "registry boot values differ" in other.reject[0], "boot value mismatch is refused clearly")
    order = join("registry-order-mismatch", fp[:67] + "0" * 64)
    check(order.reject is not None and "registry boot edits or load order differ" in order.reject[0], "registry order mismatch is refused clearly")
    state(c, 90)
    for value in (120, 0):
        (a.lab / "kiln/mods/registry-mp-test/value.txt").write_text(str(value) + "\n")
        (a.lab / "kiln/control/registry-mp.cmd").write_text("reload registry-mp-test\n")
        state(c, value)
    latest = json.loads((a.lab / "kiln/state/registry.json").read_text())["bootSync"]
    check(latest == initial, "HotReload and layer removal preserve the boot fingerprint")
    print(f"registry-mp lab: {checks} checks passed (protocol-only peers, no game client)", flush=True)
finally:
    for c in clients:
        c.s.close()
