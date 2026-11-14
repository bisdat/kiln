#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The fixture mods of boot-required-test: one boot mod per case, each with good entries for one collector and one bad
entry (a malformed file, a malformed entry or node, or a file that cannot be read). A required fixture must stop the start
with exit code 0x4B01 and a stop line that names the entry; an optional one (`"boot": true`) starts, and its collector skips
the bad entry as it does without OnBoot (or drops the mod whole where it always did: content, vfx, patches).

    fixtures.py list
    fixtures.py make <dest folder> <case> required|optional   (the entry assembly is copied in by run.sh)
    fixtures.py refuse <case>      the collector kiln.json's boot.testRefuse names for the case, or nothing
    fixtures.py fault <case>       the collector kiln.json's boot.testFault names for the case, or nothing
    fixtures.py kind <case>        stops | late | starts: what a required fixture does to the start
    fixtures.py client             the fixtures only a game client can show (guided check 08-boot.required-client)
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
    "skills-tree": "a game-tree node whose \"from\" names no node (refused by the tree build when a world loads)",
    "worldtables-merge": "a material tweak of a material the game does not have (refused by the merge when the game's data comes up)",
    "patches-comma": "a resource patch in a blocks file with comments and trailing commas (read like every blocks file: the start goes on)",
    "content-bind": "an item OnBoot stages that the world refuses at registration (lab switch boot.testFault)",
}
REFUSE = {"content-entry": "content", "vfx-entry": "vfx"}
FAULT = {"content-bind": "content"}  # kiln.json boot.testFault: the collector fails the mod's content after the publication
# what a required fixture does: "stops" before the game's entry point, "late" after it (a world or the game's data loading), "starts"
LATE = {"knowledge-entry", "skills-tree", "worldtables-merge", "content-bind"}
STARTS = {"patches-comma"}
# fixtures only a game client can show (guided check 08-boot.required-client): run.sh client <folder>
CLIENT = {
    "client-texture": "a texture override of a texture the game does not have (applied when the game's resources come up)",
    "client-vfx": "a VFX block, for a start with kiln/config/vfx.json {\"enabled\": false}",
}


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


def png_1x1():
    import struct, zlib
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\xff\xff")) + chunk(b"IEND", b"")


def make_special(dest, case):
    p = lambda *parts: os.path.join(dest, *parts)
    if case == "skills-tree":
        bad = good_node(case + "x")
        bad["name"] = "bootreq_dangling"
        bad["from"] = ["0xdeadbeef"]
        write_json(p("skills", "nodes.json"), {"format": 1, "gameTree": {"nodes": [good_node(case), bad]}})
    elif case == "worldtables-merge":
        write_json(p("blocks", "tweaks.json"), {"format": 1, "materialTweaks": {
            "bootreq-good": good_tweak(case),
            "bootreq-nowhere": {"material": "NoSuchMaterialAnywhere", "strength": {"health": 4321}}}})
    elif case == "patches-comma":
        write(p("blocks", "patches.json"), """{
  "format": 1, // comments and trailing commas, as Kiln.Blocks and every other reader of blocks files take them
  "resourcePatches": {
    "xp": {
      "resource": "82706b40-61b1-4b8f-8b23-dcec6971bda1",
      "type": "keen::BalancingTable",
      "set": { "experienceNeedPerLevel[2]": 2600, },
    },
  },
}
""")
    elif case == "client-texture":
        os.makedirs(p("assets"), exist_ok=True)
        with open(p("assets", "tint.png"), "wb") as f:
            f.write(png_1x1())
        write_json(p("assets", "assets.json"), {"format": 1, "entries": [
            {"kind": "texture", "guid": "b0071e57-0000-4000-8000-00000000c11e", "type": "keen::UiTextureResource", "file": "tint.png"}]})
    elif case == "client-vfx":
        write_json(p("blocks", "vfx.json"), {"format": 1, "vfx": {"bootreq-puff": {"particles": [{"count": 4, "life": 0.5}]}}})


def make(dest, case, role):
    if case not in CASES and case not in CLIENT:
        sys.exit("unknown case " + case)
    os.makedirs(dest, exist_ok=True)
    write_json(os.path.join(dest, "kiln.mod.json"), {
        "id": mod_id(case),
        "name": "boot-required-test fixture: " + CASES.get(case, CLIENT.get(case, "")),
        "version": "0.1.0",
        "apiVersion": "0.2",
        "entryAssembly": "BootRequiredFixture.dll",
        "side": "both" if case in CLIENT else "server",
        "clients": "mod",
        "boot": "required" if role == "required" else True,
        "advanced": True,
    })
    p = lambda *parts: os.path.join(dest, *parts)
    blob = {"kind": "blob", "file": "good.bin"}
    if case in ("skills-tree", "worldtables-merge", "patches-comma") or case in CLIENT:
        make_special(dest, case)
    elif case.startswith("assets-"):
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
    elif len(sys.argv) == 3 and sys.argv[1] == "fault":
        print(FAULT.get(sys.argv[2], ""))
    elif len(sys.argv) == 3 and sys.argv[1] == "kind":
        print("late" if sys.argv[2] in LATE else "starts" if sys.argv[2] in STARTS else "stops")
    elif len(sys.argv) == 2 and sys.argv[1] == "client":
        for c in CLIENT:
            print(c)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
