#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""roster-cap-test lab.sh: what a launcher reads after a start the roster stopped (kiln.boot stop_start).

    python3 stop_files.py <boot.json> <progress.json> <phase>

boot.json "boot": "stop" {code 0x4B02, mods [roster-cap-test]}, "exitCode" 0x4B02, a StartStopped issue for the mod;
a boot phase (129-boot, 129-boot-required) ran a boot generation ("deferred") with the mod required or not, the other
phases none. progress.json: state "failed" with the roster's reason. Prints what differs; exit 1 then."""
import json
import sys


def main():
    boot_path, progress_path, phase = sys.argv[1:4]
    bad = []
    try:
        boot = json.load(open(boot_path, encoding="utf-8"))["boot"]
    except (OSError, ValueError, KeyError) as e:
        print(f"boot.json: {e}")
        return 1
    stop = boot.get("stop") or {}
    if stop.get("code") != 0x4B02 or stop.get("mods") != ["roster-cap-test"]:
        bad.append(f"boot.stop {stop}")
    if "the roster holds at most" not in stop.get("reason", ""):
        bad.append(f"boot.stop.reason {stop.get('reason')!r}")
    if boot.get("exitCode") != 0x4B02:
        bad.append(f"boot.exitCode {boot.get('exitCode')}")
    if not any(i.get("code") == "StartStopped" and i.get("mod") == "roster-cap-test" and i.get("phase") == "stop"
               for i in boot.get("issues", [])):
        bad.append("no StartStopped issue for the mod")
    deferred = phase.startswith("129-boot")
    if bool(boot.get("deferred")) != deferred:
        bad.append(f"boot.deferred {boot.get('deferred')} (expected {deferred})")
    if deferred:
        own = [o for o in boot.get("owners", []) if o.get("id") == "roster-cap-test"]
        if not own or own[0].get("required") != (phase == "129-boot-required"):
            bad.append(f"boot.owners {own}")
    try:
        progress = json.load(open(progress_path, encoding="utf-8"))
    except (OSError, ValueError) as e:
        print(f"progress.json: {e}")
        return 1
    if progress.get("state") != "failed" or "the roster holds at most" not in (progress.get("why") or ""):
        bad.append(f"progress {progress.get('state')} {progress.get('why')!r}")
    for b in bad:
        print(b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
