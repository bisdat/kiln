#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""What npc-body-test checks against, from the lab server's game files and this machine's cook (16f lab, lab.sh):

    python3 -I lab_expect.py <server folder> <cooked mod folder> <repository> > <mod folder>/lab-expected.json

- fisherTemplateSha256: the SHA-256 of the Fisher template as the game ships it (the boot pack must leave it byte for byte);
- gameSequences: how many keen::actor::ActorSequenceResource the game has (the build adds exactly the 21 motion clones);
- clones: Fisher sequence -> its clone's id, by the native rule (native/services/quests/questpack.cpp: UUIDv5 of
  "<mod>:npcmotion:<npc>:<source>:<SHA-256 of the admitted binding vector>"), the 21 sources read from motionclose.cpp;
- sit, sleep: the authored states of enter/loop/exit and the retimed event times and length the time map gives for the
  cooked clip durations (chapter 16f §5.1: three intervals, each as long as the clip that starts it);
- model, graph: the cook's RenderModel and body graph ids.
Exits 1 when a clone id or the clone template is already one of the game's resources. Nothing here is written into the repository.
"""
import hashlib
import json
import re
import sys
import uuid
from contextlib import closing
from pathlib import Path

game, mod, repo = (Path(a) for a in sys.argv[1:4])
MOD, NPC = "npc-body-test", "figure"
sys.path.insert(0, str(repo / "tools" / "kfc"))
from kfc.archive import Kfc, ResourceKey  # noqa: E402
from kfc.util import fnv1a32  # noqa: E402

NS = uuid.UUID("6f6d2a44-53c1-5c52-9d51-0a3c6e1f2b77")  # Kiln's namespace for derived ids (BlockIds, questpack.cpp kNs)
FISHER = "e7666cd3-e530-4f9d-9586-519e4ab969dc"
TEMPLATE, SEQUENCE = fnv1a32(b"keen::ecs::TemplateResource"), fnv1a32(b"keen::actor::ActorSequenceResource")


def derive(kind, name):
    return str(uuid.uuid5(NS, f"{MOD}:{kind}:{name}"))


table = (repo / "native/services/quests/motionclose.cpp").read_text()
closure = re.findall(r'\{"([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})", "(\w+)", (\d)\}', table)
assert len(closure) == 21, closure
cache = json.loads((mod / "assets/cooked/npc-bodies.v1.json").read_text())
body = cache["bodies"][NPC]
bindings = body["bindings"]
# nlohmann's dump() of the admitted rows, in the cache's order: no spaces, UTF-8 as it is
vector = hashlib.sha256(json.dumps(bindings, separators=(",", ":"), ensure_ascii=False).encode()).hexdigest()
clones = {source: derive("npcmotion", f"{NPC}:{source}:{vector}") for source, _, _ in closure}


def phases(role, source):
    rows = sorted((b for b in bindings if b["role"] == role), key=lambda b: b["variant"])
    d = [b["durationNs"] for b in rows]
    return {"source": source, "states": [b["state"] for b in rows], "times": [0, d[0], d[0] + d[1]], "length": sum(d)}


model = [e["guid"] for e in cache["entries"] if e.get("type") == "keen::RenderModel"]
with closing(Kfc(game)) as archive:
    out = {
        "fisherTemplateSha256": hashlib.sha256(archive.read(ResourceKey(FISHER, TEMPLATE, 0))).hexdigest(),
        "gameSequences": len(list(archive.resources_of_type(SEQUENCE))),
        "clones": clones,
        "sit": phases("sit", closure[0][0]),
        "sleep": phases("sleep", closure[1][0]),
        "model": model[0] if len(model) == 1 else None,
        "graph": body["graph"],
        "vectorHash": vector,
    }
    taken = [g for g in clones.values() if archive.has(ResourceKey(g, SEQUENCE, 0))]
    taken += [g for g in (derive("npc", NPC),) if archive.has(ResourceKey(g, TEMPLATE, 0))]
assert closure[0][1] == "sit" and closure[1][1] == "sleep", closure[:2]
print(json.dumps(out, indent=1))
if taken:
    print(f"lab_expect: already the game's resources: {taken}", file=sys.stderr)
    sys.exit(1)
