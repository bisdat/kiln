#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Protocol-only Kiln peers against a dedicated disposable lab. Never starts a game client.
The native host suite covers export from real store layers; ContentJoinTests covers client application.
Run inside one wave lab session, after deploying RegistryMpTest and enabling labPort/modSync refuse.

--phase main (default): the server publishes its registry part; matching, older and order-only peers join, a values
mismatch is refused, a joined peer gets a registry layer, its HotReload and its removal.
--phase one-side: side-pack/ (a side "server" data pack writing the levelCap the shared test mod edits) is installed
too: that server has no fair counterpart on any client, so it sends no registry part and refuses no peer for it.
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
p.add_argument("--phase", choices=("main", "one-side"), default="main")
a = p.parse_args()
if not a.lab.name.startswith(("dev31-registry-mp", "dev31-review-registry-mp")):
    raise SystemExit("this fixture only drives disposable dev31-registry-mp labs")

clients = []
checks = 0
mods = [("registry-mp-test", "0.1.0", 2)]
channels = [("kiln:content", "1", False)]
key = "kiln.registry.boot"
wood = "0x2b0cf1d5"
log = a.lab / "kiln/logs/kiln.log"


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


def log_has(text, seconds=10):
    import time
    end = time.time() + seconds
    while time.time() < end:
        if text in log.read_text(errors="replace"):
            return True
        time.sleep(0.5)
    return False


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
    registry = json.loads((a.lab / "kiln/state/registry.json").read_text())
    if a.phase == "one-side":
        check("bootSync" not in registry, "registry.json has no bootSync with a side server mod on a shared field")
        check(log_has("registry boot values are not compared at joins"), "kiln.log says why the registry part is left out")
        c = join("registry-one-side-old-peer")
        check(c.welcome is not None and key not in c.welcome["props"], "WELCOME carries no registry part")
        other = join("registry-one-side-other-values", "1:" + "0" * 64 + ":" + "0" * 64)
        check(other.welcome is not None and not any("registry" in w for w in other.welcome["warnings"]),
              "a peer with other registry values joins under refuse, without a registry warning")
    else:
        c = join("registry-old-peer")
        check(c.welcome is not None, "older peer without registry extension joins under refuse")
        fp = c.welcome["props"].get(key)
        check(fp is not None and len(fp) == 131 and fp.startswith("1:"), "WELCOME carries resolved registry values and order")
        initial = registry["bootSync"]
        check(fp == "1:" + initial["values"] + ":" + initial["order"], "native report reaches the real Host/Network seam")
        same = join("registry-matching-peer", fp)
        check(same.welcome is not None and not same.welcome["warnings"], "matching registry fingerprint joins")
        other = join("registry-values-mismatch", "1:" + "0" * 64 + fp[66:])
        check(other.reject is not None and "registry boot values differ" in other.reject[0], "boot value mismatch is refused clearly")
        order = join("registry-order-only", fp[:67] + "0" * 64)
        check(order.welcome is not None and not order.welcome["warnings"], "same values in another order join under refuse, without a warning")
        check(log_has("registry boot edits or load order differ"), "the server's log names the order difference")
        state(c, 90)
        for value in (120, 0):
            (a.lab / "kiln/mods/registry-mp-test/value.txt").write_text(str(value) + "\n")
            (a.lab / "kiln/control/registry-mp.cmd").write_text("reload registry-mp-test\n")
            state(c, value)
        latest = json.loads((a.lab / "kiln/state/registry.json").read_text())["bootSync"]
        check(latest == initial, "HotReload and layer removal preserve the boot fingerprint")
    print(f"registry-mp lab ({a.phase}): {checks} checks passed (protocol-only peers, no game client)", flush=True)
finally:
    for c in clients:
        c.s.close()
