#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The staged Kiln update at the proxy's entry gate, with the real dinput8.dll (test-staged-gate.sh).

A stand-in enshrouded_server.exe imports dinput8.dll, so the loader maps Kiln's proxy and its gate applies kiln/staged
before it loads kiln-core.dll (a stand-in that writes which version it is). The update renames the running dinput8.dll
and then the folder that holds it; the next start (the new proxy, not confirmed) rolls back by itself, once; a requested
rollback goes forward again; kiln/data stays byte for byte; a Kiln rollback over a receipt another installer wrote is
refused. Natively on Windows (python gate_test.py <scratch> <bin>) or under Wine (--wine, as test-staged-gate.sh runs
it). <bin> holds enshrouded_server.exe, dinput8.dll, core-A.dll and core-B.dll."""
import hashlib, json, os, shutil, subprocess, sys

scratch, binary = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
wine = "--wine" in sys.argv[3:]
game = os.path.join(scratch, "game")
kiln = os.path.join(game, "kiln")
staged = os.path.join(kiln, "staged")
checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
    print(("ok   " if cond else "FAIL ") + what)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sha_of(path):
    return sha(open(path, "rb").read()) if os.path.isfile(path) else "(missing)"


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def last_core():
    p = os.path.join(kiln, "core-ran.txt")
    return open(p).read().split()[-1] if os.path.isfile(p) else ""


def results():
    d = os.path.join(staged, "results")
    return [json.load(open(os.path.join(d, n))) for n in sorted(os.listdir(d)) if n.endswith(".json")] if os.path.isdir(d) else []


def tree(d):
    return {os.path.relpath(os.path.join(r, n), d): sha_of(os.path.join(r, n)) for r, _, fs in os.walk(d) for n in fs}


def start(label):
    exe = os.path.join(game, "enshrouded_server.exe")
    env = dict(os.environ, WINEDLLOVERRIDES="dinput8=n,b;winedbg.exe=d", WINEDEBUG="-all") if wine else None
    p = subprocess.run(["wine", exe] if wine else [exe], cwd=game, env=env, capture_output=True, text=True, timeout=120)
    print(f"---- {label} (exit {p.returncode}): {p.stdout.strip()}")
    log = os.path.join(kiln, "logs", "proxy.log")
    for line in (open(log, encoding="utf-8", errors="replace").read().splitlines() if os.path.isfile(log) else []):
        if "staged" in line:
            print("     proxy.log: " + line)
    return p.stdout


shutil.rmtree(game, ignore_errors=True)
os.makedirs(kiln)
shutil.copy(os.path.join(binary, "enshrouded_server.exe"), game)
v1 = open(os.path.join(binary, "dinput8.dll"), "rb").read()
v2 = v1 + b"KILN-STAGED-GATE-OVERLAY-v2"  # the same proxy, other bytes (a PE overlay)
put(os.path.join(game, "dinput8.dll"), v1)
shutil.copy(os.path.join(binary, "core-A.dll"), os.path.join(kiln, "kiln-core.dll"))
receipt1 = b'{"format":1,"side":"server","runtime":{"version":"0.2.0-dev.30.9"},"files":[]}'
receipt2 = b'{"format":1,"side":"server","runtime":{"version":"0.2.0-dev.30.10"},"files":[]}'
put(os.path.join(kiln, ".install.json"), receipt1)
put(os.path.join(kiln, "data", "kiln-admin", "admin-token"), "token")
put(os.path.join(kiln, "data", "kiln-network", "server-id"), "srv-1")
data_before = tree(os.path.join(kiln, "data"))

# staged as Kiln.Admin's update_stage writes it: files/, then staged.json
s = os.path.join(staged, "kiln")
core_b = open(os.path.join(binary, "core-B.dll"), "rb").read()
files = {"dinput8.dll": v2, "kiln/kiln-core.dll": core_b, "kiln/.install.json": receipt2}
for rel, data in files.items():
    put(os.path.join(s, "files", *rel.split("/")), data)
put(os.path.join(s, "staged.json"), json.dumps({
    "format": 1, "kind": "kiln", "version": "0.2.0-dev.30.10", "from": "0.2.0-dev.30.9", "by": "gate_test",
    "base": {"receipt": sha(receipt1)}, "remove": [],
    "files": [{"path": rel, "sha256": sha(data), "size": len(data)} for rel, data in files.items()]}))

out = start("start 1: the staged update")
check("main reached" in out, "start 1 reached the game's main")
check(sha_of(os.path.join(game, "dinput8.dll")) == sha(v2), "start 1: dinput8.dll is the new proxy (the running one renamed away)")
check(sha_of(os.path.join(kiln, "kiln-core.dll")) == sha(core_b), "start 1: kiln-core.dll is the new core")
check(last_core() == "core-B", "start 1: the proxy loaded the new core in the same start")
check(sha_of(os.path.join(staged, "previous", "kiln", "files", "dinput8.dll")) == sha(v1), "start 1: the running proxy is kept")
check(not os.path.exists(os.path.join(staged, "tx")), "start 1: no transaction left open (the folder holding the running proxy was renamed)")
check(not os.path.exists(os.path.join(staged, "kiln")), "start 1: the staged update is gone")
check(json.load(open(os.path.join(staged, "probation", "kiln.json")))["state"] == "booting", "start 1: probation booting")
r = results()
check(len(r) == 1 and r[0]["ok"] and r[0]["op"] == "update" and r[0]["by"] == "proxy", "start 1: one result (update, ok, by the proxy)")

out = start("start 2: not confirmed: the automatic rollback, by the new proxy")
check("main reached" in out, "start 2 reached main")
check(sha_of(os.path.join(game, "dinput8.dll")) == sha(v1), "start 2: the old proxy again (the running new one renamed away)")
check(last_core() == "core-A", "start 2: the old core booted")
check(not os.path.exists(os.path.join(staged, "probation", "kiln.json")), "start 2: no probation for the version it went back to")
check(not os.path.exists(os.path.join(staged, "tx")), "start 2: no transaction left open")
check(len(results()) == 2 and results()[1]["op"] == "auto-rollback" and results()[1]["ok"], "start 2: the automatic rollback's result")

start("start 3: nothing to do (never in a loop)")
check(sha_of(os.path.join(game, "dinput8.dll")) == sha(v1) and last_core() == "core-A" and len(results()) == 2, "start 3: unchanged")

put(os.path.join(staged, "rollback", "kiln.json"), "{}")
start("start 4: a rollback asked for by hand")
check(sha_of(os.path.join(game, "dinput8.dll")) == sha(v2) and last_core() == "core-B", "start 4: the new version again")
check(not os.path.exists(os.path.join(staged, "rollback", "kiln.json")), "start 4: the request is gone")

# kilnctl or the launcher installs Kiln: the kept version is not put back over it
put(os.path.join(kiln, ".install.json"), b'{"format":1,"side":"server","runtime":{"version":"0.2.0-dev.30.11"},"installer":"kilnctl"}')
put(os.path.join(staged, "rollback", "kiln.json"), "{}")
start("start 5: a rollback over another installer's Kiln")
check(sha_of(os.path.join(game, "dinput8.dll")) == sha(v2) and last_core() == "core-B", "start 5: refused, nothing changed")
check(not results()[-1]["ok"] and "Kiln changed since" in results()[-1].get("error", ""), "start 5: the refusal's result says why")

check(tree(os.path.join(kiln, "data")) == data_before, "kiln/data byte for byte")
print(f"gate_test: {checks - fails} of {checks} checks passed")
sys.exit(1 if fails else 0)
