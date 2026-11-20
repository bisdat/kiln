#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""native-test on a lab dedicated server: the native loader's phases, and the Shroudtopia first-pass wait (compat rule
action "defer", inbox 26c). The real kiln-core loads every native; compathost (native/plugins/compat/tests) cannot,
since it stands in for kiln-core. Builds nothing: run first

    KILN_OUT=... native/build.sh kiln-native-test kiln-native-plain kiln-native-plugin kiln-native-mod kiln-native-hello \
        compat-shroudtopia_lab kiln-shroudtopia
    KILN_OUT=... tools/deploy-server.sh <lab dir> --policy any        (a lab from tools/lab/lab.sh, never the live server)

  lab.py stage <lab dir> normal|first-pass|timeout   write <lab>/library and <lab>/kiln/profile.json (configDir kiln/config)
  lab.py check <lab dir> normal|first-pass|timeout   after a start and a stop (tools/lab/lab.sh stop: SIGINT)

normal      stage.py's CASES (early/boot/world, with and without init, failing, faulting, unsupported, dllmain, cycles),
            the native-mod sample (boot, init) and the native-hello plugin through a profile. kiln-shroudtopia runs (Kiln's
            own plugin) with no Shroudtopia mod listed: nothing waits.
first-pass  normal + item-skill-text (compat-tests' shroudtopia_lab.dll under that id) in the profile's Shroudtopia list,
            an Auto Doors stand-in (a plain early native whose package has AutoDoors/bin/ad_core.dll, the shipped rule's
            subject) and a plain boot native that requires it; bootDelay 3000 (a player's). The stand-in loads after the
            probe's Activate, off the game's main thread (the boot phase is done before it).
timeout     first-pass + bootDelay 20000 and a player's rule (same id) with max_wait_ms 5000: the stand-in loads ~5 s after
            the host started, with a warning, before the probe's Activate.
"""
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "../../.."))
OUT = os.path.abspath(os.environ.get("KILN_OUT", os.path.join(REPO, "native/build")))
BIN = os.path.join(OUT, "cmake", "bin")
RUNS = ("normal", "first-pass", "timeout")
RULE_ID = "auto-doors-after-shroudtopia-first-pass"

_spec = importlib.util.spec_from_file_location("native_test_stage", os.path.join(HERE, "stage.py"))
stage_py = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(stage_py)


def winpath(p):
    return "Z:" + os.path.abspath(p).replace("/", "\\")


def plain_native(lib, mid, fields, extra=None):
    plain = os.path.join(BIN, "samples", "native-test", "native-plain.dll")
    d = os.path.join(lib, "native", mid)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy(plain, os.path.join(d, mid + ".dll"))
    man = {"format": 1, "id": mid, "name": mid, "version": "1.0.0", "dlls": [mid + ".dll"]}
    man.update(fields)
    json.dump(man, open(os.path.join(d, "native.json"), "w"), indent=2)
    for rel in extra or []:
        os.makedirs(os.path.dirname(os.path.join(d, rel)), exist_ok=True)
        shutil.copy(plain, os.path.join(d, rel))
    return {"id": mid, "version": "1.0.0", "path": winpath(d)}


def stage(lab, run):
    kiln, lib = os.path.join(lab, "kiln"), os.path.join(lab, "library")
    cfg = os.path.join(kiln, "config")
    shutil.rmtree(lib, ignore_errors=True)
    for f in (os.path.join(cfg, "compat-rules.json"), os.path.join(cfg, "shroudtopia", "shroudtopia.json"),
              os.path.join(kiln, "state", "native.json"), os.path.join(kiln, "logs", "kiln.log")):
        if os.path.exists(f):
            os.remove(f)
    env = dict(os.environ, KILN_OUT=OUT)
    script = os.path.join(HERE, "stage.py")
    subprocess.run([sys.executable, "-I", script, "library", lib], check=True, env=env)
    subprocess.run([sys.executable, "-I", script, "profile", kiln, lib, "--config-dir", cfg], check=True, env=env)
    path = os.path.join(kiln, "profile.json")
    prof = json.load(open(path))
    d = os.path.join(lib, "native", "native-mod-sample")
    os.makedirs(d)
    for f in ("kiln-native-mod.dll", "native.json"):
        shutil.copy(os.path.join(BIN, "samples", "native-mod", f), d)
    prof["mods"]["native"].append({"id": "native-mod-sample", "version": "1.0.0", "path": winpath(d)})
    shutil.copy(os.path.join(BIN, "kiln-native-hello.dll"), os.path.join(lib, "plugin"))
    prof["mods"]["plugin"].append({"id": "kiln-native-hello", "path": winpath(os.path.join(lib, "plugin", "kiln-native-hello.dll"))})
    if run != "normal":
        prof["mods"]["native"].append(plain_native(lib, "auto-doors-standin", {"phase": "early", "side": "both"},
                                                   ["AutoDoors/bin/ad_core.dll"]))
        prof["mods"]["native"].append(plain_native(lib, "auto-doors-successor",
                                                   {"phase": "boot", "side": "both", "requires": ["auto-doors-standin"]}))
        st = os.path.join(lib, "item-skill-text")
        os.makedirs(st)
        json.dump({"id": "item-skill-text"}, open(os.path.join(st, "mod.json"), "w"))
        shutil.copy(os.path.join(BIN, "compat-tests", "shroudtopia_lab.dll"), os.path.join(st, "item-skill-text.dll"))
        prof["mods"]["shroudtopia"] = [{"id": "item-skill-text", "path": winpath(st), "enabled": True}]
        os.makedirs(os.path.join(cfg, "shroudtopia"), exist_ok=True)
        probe = {"active": True, "flag": True, "count": 5, "ratio": 1.5, "title": "kiln", "big": 12345678901,
                 "bools": [True, False], "ints": [1, 2, 3], "floats": [1.5, 2.5], "strings": ["a", "b"], "bigs": [1, 2]}
        json.dump({"active": True, "bootDelay": 20000 if run == "timeout" else 3000, "updateDelay": 500,
                   "enableLogging": True, "logLevel": "INFO", "mods": {"Kiln Lab Probe": probe}},
                  open(os.path.join(cfg, "shroudtopia", "shroudtopia.json"), "w"), indent=2)
    if run == "timeout":
        shipped = json.load(open(os.path.join(REPO, "native/core/compat-rules.json")))
        rule = dict(next(r for r in shipped["rules"] if r.get("id") == RULE_ID))
        rule["max_wait_ms"] = 5000
        json.dump({"format": 1, "rules": [rule]}, open(os.path.join(cfg, "compat-rules.json"), "w"), indent=2)
    json.dump(prof, open(path, "w"), indent=2)
    print(f"native-test lab: run '{run}' staged in {lab} ({len(prof['mods']['native'])} natives, "
          f"{len(prof['mods']['plugin'])} plugins, {len(prof['mods'].get('shroudtopia', []))} Shroudtopia mod)")
    return 0


def check(lab, run):
    kiln = os.path.join(lab, "kiln")
    log = open(os.path.join(kiln, "logs", "kiln.log"), encoding="utf-8", errors="replace").read().replace("\r", "")
    lines = log.splitlines()
    state = {m["id"]: m for m in json.load(open(os.path.join(kiln, "state", "native.json")))["mods"]}
    fails = []

    def ok(cond, what):
        print(("PASS " if cond else "FAIL ") + what)
        if not cond:
            fails.append(what)

    def first(pattern):
        return next(((i, l) for i, l in enumerate(lines) if re.search(pattern, l)), (None, None))

    expected = {mid: exp for mid, (_, _, exp) in stage_py.CASES.items()}
    expected["native-mod-sample"] = "loaded"
    if run != "normal":
        expected.update({"auto-doors-standin": "loaded", "auto-doors-successor": "loaded"})
    for mid, exp in expected.items():
        got = state.get(mid, {}).get("state")
        ok(got == exp, f"{mid}: {got} (expected {exp})")
    ok(set(state) == set(expected), f"native.json lists exactly the staged natives ({len(state)})")
    passes = len(re.findall(r"native-test \S+: .* PASS", log))
    failed = [l for l in lines if re.search(r"native-test \S+: .* FAIL", l)]
    ok(passes > 0 and not failed, f"native-test's checks: {passes} PASS, {len(failed)} FAIL {failed[:2]}")
    ok(first(r"hello from native-mod-sample 1\.0\.0 \(server, hooks on\)")[1] is not None, "native-mod-sample's init ran")
    ok(first(r"plugin kiln-native-hello\.dll loaded")[1] is not None, "the native-hello plugin loaded")
    ok(first(r"native mod native-test-world .* loaded \(world")[1] is not None, "the world phase loaded native-test-world")
    code_file = os.path.join(kiln, "state", "exit-code.txt")
    code = open(code_file).read().strip() if os.path.exists(code_file) else "?"
    ok(code == "0", f"the server exited with code {code} after SIGINT")
    waits = [l for l in lines if "waits for Shroudtopia" in l or "waits for native" in l]
    if run == "normal":
        ok(not waits, "nothing waits for Shroudtopia")
        print(f"{len(fails)} FAIL")
        return 1 if fails else 0
    ok(any("native auto-doors-standin waits for Shroudtopia's first pass (item-skill-text is on)" in l for l in waits),
       "the Auto Doors stand-in waits for Shroudtopia's first pass")
    ok(any("native auto-doors-successor waits for native auto-doors-standin" in l for l in waits), "its successor (requires) waits too")
    ok(all("auto-doors-" in l for l in waits), "no other native waits")
    i_act, act = first(r"Activating: Kiln Lab Probe")
    i_std, std = first(r"native mod auto-doors-standin 1\.0\.0 loaded")
    i_suc, suc = first(r"native mod auto-doors-successor 1\.0\.0 loaded")
    i_boot, _ = first(r"native: phase boot: ")
    ok(std is not None and suc is not None and i_suc > i_std, "the stand-in loaded, then its successor")
    ok(i_boot is not None and i_std is not None and i_boot < i_std, "the boot phase was done before the stand-in loaded (no main-thread wait)")
    if run == "first-pass":
        ok(first(r"native auto-doors-standin started after Shroudtopia's first pass, \d+ ms")[1] is not None, "released by the first pass")
        ok(act is not None and i_act < i_std, "the probe's Activate ran before the stand-in loaded")
    else:
        ok(first(r"auto-doors-standin: Shroudtopia's first pass did not finish within 5000 ms; starting anyway")[1] is not None and
           first(r"native auto-doors-standin started after the Shroudtopia wait timed out, \d+ ms")[1] is not None, "released by the timeout")
        ok(first(rf"the player's rule '{RULE_ID}' replaces the shipped one")[1] is not None, "the player's rule replaced the shipped one")
        ok(act is not None and i_act > i_std, "the stand-in loaded before the probe's Activate")
    print(f"{len(fails)} FAIL")
    return 1 if fails else 0


def main(argv):
    if len(argv) != 4 or argv[1] not in ("stage", "check") or argv[3] not in RUNS:
        print(__doc__)
        return 2
    lab = os.path.abspath(argv[2])
    return stage(lab, argv[3]) if argv[1] == "stage" else check(lab, argv[3])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
