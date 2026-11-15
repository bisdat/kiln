#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""content-stop-test lab.sh: what a launcher reads after a start the whole build of the mods' NPCs and quests stopped.

    python3 check_stop.py <content-refused.json> <boot.json> <progress.json> <mod> <why part>

content-refused.json: refused, the mod, exitCode 0x4B03, unixExitStatus 3, restart false, bootStop true, why holding
<why part>. boot.json "boot": "stop" {code 0x4B03, mods [<mod>], by kiln.knowledge's owner}, "exitCode" 0x4B03, a
StartStopped issue for the mod. progress.json: state "failed" with the reason. Prints what differs; exit 1 then."""
import json
import sys


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def main():
    refused_path, boot_path, progress_path, mod, why = sys.argv[1:6]
    bad = []
    try:
        r = load(refused_path)
        if not (r.get("refused") is True and r.get("mods") == [mod] and r.get("exitCode") == 0x4B03 and r.get("unixExitStatus") == 3
                and r.get("restart") is False and r.get("bootStop") is True and why in r.get("why", "")
                and "Remove or update the mods named here" in r.get("advice", "")):
            bad.append(f"content-refused.json {r}")
    except (OSError, ValueError) as e:
        bad.append(f"content-refused.json: {e}")
    try:
        boot = load(boot_path)["boot"]
        stop = boot.get("stop") or {}
        if stop.get("code") != 0x4B03 or stop.get("mods") != [mod] or why not in stop.get("reason", ""):
            bad.append(f"boot.stop {stop}")
        if boot.get("exitCode") != 0x4B03:
            bad.append(f"boot.exitCode {boot.get('exitCode')}")
        if not any(i.get("code") == "StartStopped" and i.get("mod") == mod and i.get("phase") == "stop" for i in boot.get("issues", [])):
            bad.append("no StartStopped issue for the mod")
    except (OSError, ValueError, KeyError) as e:
        bad.append(f"boot.json: {e}")
    try:
        progress = load(progress_path)
        if progress.get("state") != "failed" or why not in (progress.get("why") or ""):
            bad.append(f"progress {progress.get('state')} {progress.get('why')!r}")
    except (OSError, ValueError) as e:
        bad.append(f"progress.json: {e}")
    for b in bad:
        print(b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
