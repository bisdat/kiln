#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The fixture mods of boot-required-test: one boot mod per case, each with good entries for one collector and one bad
entry (a malformed file, a malformed entry or node, or a file that cannot be read). A required fixture must stop the start
with exit code 0x4B01 and a stop line that names the entry; an optional one (`"boot": true`) starts, and its collector skips
the bad entry as it does without OnBoot (or drops the mod whole where it always did: content, vfx, patches).

    fixtures.py list
    fixtures.py make <dest folder> <case> required|optional   (the entry assembly is copied in by run.sh)
    fixtures.py refuse <case>      the collector kiln.json's boot.testRefuse names for the case, or nothing
"""
import json
import os
import sys

CASES = {
    "assets-file": "a malformed assets/assets.json",
    "assets-entry": "an assets.json entry with a bad GUID",
    "assets-load": "an assets.json entry whose file is missing",
    "worldtables-file": "a blocks file that is not JSON",
    "worldtables-entry": "a material tweak with a bad strength",
    "worldtables-load": "a blocks file that cannot be read",
    "skills-file": "a skills file that is not JSON",
    "skills-entry": "a game-tree node with a bad GUID",
    "skills-load": "a skills file that cannot be read",
    "knowledge-file": "a quests file that is not JSON",
    "knowledge-entry": "a knowledge object of an unknown type (refused by the build after the entry point)",
    "knowledge-load": "a quests file that cannot be read",
    "content-entry": "an unreadable content document (lab switch boot.testRefuse)",
    "vfx-entry": "an unreadable vfx document (lab switch boot.testRefuse)",
    "patches-entry": "a resource patch of an unknown type",
}
REFUSE = {"content-entry": "content", "vfx-entry": "vfx"}


def mod_id(case):
    return "boot-req-" + case


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def write_json(path, doc):
    write(path, json.dumps(doc, indent=2) + "\n")


def locked(path, doc):
    """A well-formed file nobody may read (mode 000): the collector's read of it fails."""
    write_json(path, doc)
    os.chmod(path, 0)


def node_guid(case, n):
    h = sum(ord(c) * (i + 1) for i, c in enumerate(case)) & 0xFFFF
    return "b007%04x-%04x-4000-8000-0000000000%02x" % (h, n, len(case))


def good_tweak(case):
    return {"material": "StoneFancy", "strength": {"health": 4000 + len(case)}}


def good_node(case):
    return {
        "guid": node_guid(case, 1),
        "name": "bootreq_" + case.replace("-", "_"),
        "type": "Attribute",
        "class": "Warrior",
        "like": "0x0b61f08f",
        "from": ["0x0b61f08f"],
        "offset": [150, -120 - 10 * len(case)],
        "cost": 1,
        "configs": {"0x0a0c87e7": 1},
    }


def good_knowledge(case):
    return {"format": 1, "knowledge": {"bootreq-" + case + "-flag": {"type": "bool", "scope": "player"}}}


def make(dest, case, role):
    if case not in CASES:
        sys.exit("unknown case " + case)
    os.makedirs(dest, exist_ok=True)
    write_json(os.path.join(dest, "kiln.mod.json"), {
        "id": mod_id(case),
        "name": "boot-required-test fixture: " + CASES[case],
        "version": "0.1.0",
        "apiVersion": "0.2",
        "entryAssembly": "BootRequiredFixture.dll",
        "side": "server",
        "clients": "mod",
        "boot": "required" if role == "required" else True,
        "advanced": True,
    })
    p = lambda *parts: os.path.join(dest, *parts)
    blob = {"kind": "blob", "file": "good.bin"}
    if case.startswith("assets-"):
        write(p("assets", "good.bin"), "boot-required-test: a good blob of " + case + "\n")
        if case == "assets-file":
            write(p("assets", "assets.json"), '{ "format": 1, "entries": [ { "kind": "blob", "file": "good.bin" }, not json\n')
        elif case == "assets-entry":
            write_json(p("assets", "assets.json"), {"format": 1, "entries": [blob, {"kind": "resource", "guid": "not-a-guid", "type": "0x0", "file": "good.bin"}]})
        else:
            write_json(p("assets", "assets.json"), {"format": 1, "entries": [blob, {"kind": "blob", "file": "missing.bin"}]})
    elif case.startswith("worldtables-"):
        good = {"format": 1, "materialTweaks": {"bootreq-good": good_tweak(case)}}
        if case == "worldtables-file":
            write_json(p("blocks", "good.json"), good)
            write(p("blocks", "zz-bad.json"), '{ "format": 1, "materialTweaks": { not json\n')
        elif case == "worldtables-entry":
            good["materialTweaks"]["bootreq-bad"] = {"material": "StoneFancy", "strength": {"hardness": "squishy"}}
            write_json(p("blocks", "tweaks.json"), good)
        else:
            write_json(p("blocks", "good.json"), good)
            locked(p("blocks", "zz-locked.json"), {"format": 1, "materialTweaks": {"bootreq-locked": good_tweak(case + "x")}})
    elif case.startswith("skills-"):
        good = {"format": 1, "gameTree": {"nodes": [good_node(case)]}}
        if case == "skills-file":
            write_json(p("skills", "good.json"), good)
            write(p("skills", "zz-bad.json"), '{ "format": 1, "gameTree": { not json\n')
        elif case == "skills-entry":
            good["gameTree"]["nodes"].append({"guid": "not-a-guid", "name": "bootreq_bad"})
            write_json(p("skills", "nodes.json"), good)
        else:
            write_json(p("skills", "good.json"), good)
            locked(p("skills", "zz-locked.json"), {"format": 1, "gameTree": {"nodes": []}})
    elif case.startswith("knowledge-"):
        write_json(p("quests", "good.json"), good_knowledge(case))
        if case == "knowledge-file":
            write(p("quests", "zz-bad.json"), '{ "format": 1, "knowledge": { not json\n')
        elif case == "knowledge-entry":
            write_json(p("quests", "zz-bad.json"), {"format": 1, "knowledge": {"bootreq-bad-flag": {"type": "weird"}}})
        else:
            locked(p("quests", "zz-locked.json"), good_knowledge(case + "x"))
    elif case == "patches-entry":
        write_json(p("blocks", "patches.json"), {"format": 1, "resourcePatches": {
            "good-xp": {"format": 1, "resource": "82706b40-61b1-4b8f-8b23-dcec6971bda1", "type": "keen::BalancingTable",
                        "set": {"experienceNeedPerLevel[1]": 2400}},
            "bad-type": {"format": 1, "resource": "82706b40-61b1-4b8f-8b23-dcec6971bda1", "type": "keen::NoSuchTypeAnywhere",
                         "set": {"value": 1}},
        }})
    # content-entry, vfx-entry: no files; the lab switch hands the collector an unreadable document of the mod


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "list":
        for c, what in CASES.items():
            print(c + "\t" + what)
    elif len(sys.argv) == 5 and sys.argv[1] == "make":
        make(sys.argv[2], sys.argv[3], sys.argv[4])
    elif len(sys.argv) == 3 and sys.argv[1] == "refuse":
        print(REFUSE.get(sys.argv[2], ""))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
