#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes roster-cap-test's quests/roster.json and lab.json for one boot of lab.sh:

    python3 gen_roster.py <phase> <mod dir>

phase: 64 (29 new entries, 64 with the game's 35), 65 (30), 129 (94: the start stops), 65-refused (30, with
kiln/control/roster.fail-install: the start stops), 128 (93), 128-restart (93, after a restart), 128-control (93, with
the negative control kiln/control/roster.no-pages). Each entry: a clone of one game villager with its own world bool
knowledge "rc_<i>" as its unlock; GUIDs fixed (uuid5 of the entry's number).

    python3 gen_roster.py fixture <mods/tests/roster-cap-fixture>

writes the guided check's fixture (17-roster-client): 93 entries as for 128, other GUIDs (its own mod id), and the
entries 63 and 65 (the mod's rc_npc_028 and rc_npc_030) clones of the fisher trader with a summon tool each, so a player can
summon them and send them to a bed.
"""
import json
import os
import sys
import uuid

NEW = {"64": 29, "65": 30, "129": 94, "65-refused": 30, "128": 93, "128-restart": 93, "128-control": 93, "fixture": 93}
NS = uuid.UUID("3b1c2a4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d")


def main():
    phase, mod = sys.argv[1], sys.argv[2]
    n = NEW[phase]
    knowledge = {f"rc_{i}": {"scope": "world"} for i in range(n)}
    npcs = {}
    fixture = phase == "fixture"
    mod_id = "roster-cap-fixture" if fixture else "roster-cap-test"
    for i in range(n):
        g = uuid.uuid5(NS, f"{mod_id}:{i}")
        npc = {
            "template": "Townsfolk_Female_villager_01_HannahReed",
            "clone": {"guid": str(g), "name": f"Kiln_{'RosterFix' if fixture else 'RosterCap'}_{i:03d}"},
            "names": {"en": f"Roster entry {35 + i}"},
            "roster": {"unlock": f"rc_{i}", "roamingRange": 8},
        }
        if fixture and i in (28, 30):  # entries 63 and 65: summoned and sent to a bed by the player
            npc["template"] = "NPC_Workshop_Fisher_Trader"
            npc["roster"]["summonItem"] = {
                "guid": str(uuid.uuid5(NS, f"{mod_id}:{i}.tool")),
                "name": f"Kiln_RosterFix_{i:03d}_Tool",
                "base": "NPC_Townsfolk_Male_Villager_08_fisher_Trader",
                "texts": {"en": {"caption": f"Roster entry {35 + i}", "primaryAction": "Call here, or send to a bed"}},
                "refs": {"equipment.placedEntity": str(g)},
            }
        npcs[f"rc_npc_{i:03d}"] = npc  # the build orders a mod's NPCs by key: padded, that is the number's order
    os.makedirs(os.path.join(mod, "quests"), exist_ok=True)
    with open(os.path.join(mod, "quests", "roster.json"), "w", encoding="utf-8") as f:
        json.dump({"format": 1, "comment": f"{mod_id}, {phase}: {n} new roster entries (written by mods/tests/roster-cap-test/gen_roster.py)",
                   "knowledge": knowledge, "npcs": npcs}, f, indent=1)
        f.write("\n")
    if not fixture:
        with open(os.path.join(mod, "lab.json"), "w", encoding="utf-8") as f:
            json.dump({"phase": phase, "new": n}, f)


if __name__ == "__main__":
    main()
