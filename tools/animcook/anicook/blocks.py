# SPDX-License-Identifier: MIT
"""Cook steps for the animations and bodies a mod's blocks/*.json defines (docs/kiln/modding/npc-blocks.md).

A block that brings its own files says so with a path in the mod's folder:

    "animations": { "golem-moves": { "clips": "models/golem.glb", "idle": "Idle", "walk": "Walk", "dead": "Die",
                                      "hit": { "clip": "Flinch" }, "states": { "slam": { "clip": "Slam", "speed": 1.2 } } } },
    "bodies":     { "golem-body":  { "model": "models/golem.glb", "slots": { "HandR": "hand_r" } } }

Both tools read this module when they load a recipe, so a mod's assets/cook.json needs no step for them: the skeleton, clips and graph steps
(animcook) and the model step (modelcook) are added. A step of the recipe with the same name says more, and wins. A block written in code is not
visible here: Kiln.Blocks refuses one that brings files, and says to put it in blocks/*.json.

Names, and so the GUIDs that follow from <mod>:<kind>:<name>, are what Kiln.Blocks expects (Npc/NpcCook.cs):
- skeleton: the block's "skeleton", else the file's name without its extension;
- clip: "<skeleton>.<animation>", with ".once" when it plays once (the engine's moving and idle states loop, every other state plays once), ".rm" with
  root motion;
- graph: the animation block's name, or "<animation>.<body>" for a body on the same skeleton with slots: slots are the graph's;
- model: the model file's path in the mod without the extension, as a worn look's (cook/worn.py in tools/modelcook).

A body's "cloth" and "chains" (Kiln.Blocks' ModelClothBlock and ModelChainBlock) are its model's physics: add_to hands them to modelcook as the
recipe's "blocksPhysics", which reads them as it reads assets/physics.json (tools/modelcook/cook/physics.py), and refuses a model both name.

An animation with "retarget" brings clips authored on a rig of their own and names the "skeleton" they play on, a body's: its clips step pairs the rigs'
joints (anicook/retarget.py) and the skeleton stays the body's.
"""
import json
import os
import re
import warnings
from pathlib import Path

ENGINE_FIELDS = {"idle": "Idle", "walk": "Walk", "run": "Run", "sprint": "Sprint", "idleCombat": "IdleCombat", "fall": "Fall", "idleSneak": "IdleSneak",
                 "sneak": "Sneak", "dead": "Dead", "stun": "Stun"}
LOOPED = {"Idle", "Walk", "Run", "Sprint", "IdleCombat", "IdleSneak", "Sneak"}
DERIVED = ("IdleCombat", "Fall")  # states a graph gets from Idle when the block leaves them out (and Stagger, Stun, Dead: see plan())
SECTIONS = ("animations", "bodies")
INLINE = ("creatures", "npcs")


class BlocksError(Exception):
    pass


def _relaxed(text):
    """JSON with // comments and trailing commas, as the block loader reads it."""
    out, i, n, in_str = [], 0, len(text), False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
            continue
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        else:
            out.append(c)
        i += 1
    return re.sub(r",(\s*[}\]])", r"\1", "".join(out))


def stem(path):
    """A file's name without its extension: the default skeleton name."""
    return Path(path).name.rsplit(".", 1)[0]


def model_name(path):
    """What a body's model GUID follows from: its path in the mod without the extension (as tools/modelcook's worn looks)."""
    p = Path(path).as_posix()
    return p[:p.rfind(".")] if "." in Path(p).name else p


def _slot(name):
    """A slot as the graph names it ("HandR"), however JSON cased it ("handR")."""
    from .graphgen import SLOTS
    return next((s for s in SLOTS if s.lower() == str(name).lower()), name)


def clip_spec(v, role=None):
    """A state's or layer's source as a dict: {"clip": name, ...} or {"blend": {...}}. A string is a glTF animation's name."""
    if isinstance(v, str):
        return {"clip": v}
    if isinstance(v, dict) and ("clip" in v or "blend" in v):
        return v
    raise BlocksError(f"{role or 'a state'}: a glTF animation's name, or {{\"clip\": ...}} or {{\"blend\": ...}}")


def clip_key(skeleton, animation, loop_default, c):
    """The clip's name in the cook, and its clips-step entry."""
    loop = c.get("loop", loop_default)
    key = f"{skeleton}.{animation}" + ("" if loop else ".once")
    entry = {"animation": animation, "loop": bool(loop)}
    if c.get("rootMotion"):
        key += ".rm"
        entry["rootMotion"] = True
    if c.get("events"):
        entry["events"] = c["events"]
    return key, entry


def read(mod_dir):
    """(file, name, kind, block) of every animation and body of the mod's blocks/*.json, inline ones of creatures and NPCs as "<name>.animation", "<name>.body"."""
    out = []
    for f in sorted((Path(mod_dir) / "blocks").glob("*.json")):
        try:
            doc = json.loads(_relaxed(f.read_text()))
        except ValueError:
            continue  # the block loader and kiln lint say what is wrong with it
        if not isinstance(doc, dict):
            continue
        for kind, section in (("animation", "animations"), ("body", "bodies")):
            for name, b in (doc.get(section) or {}).items():
                if isinstance(b, dict):
                    out.append((f.name, name, kind, b))
        for section in INLINE:
            for name, c in (doc.get(section) or {}).items():
                if not isinstance(c, dict):
                    continue
                for kind in ("animation", "body"):
                    if isinstance(c.get(kind), dict):
                        out.append((f.name, f"{name}.{kind}", kind, c[kind]))
    return out


def _in_mod(mod_dir, f):
    root = Path(mod_dir).resolve()
    p = (root / f).resolve()
    return p.is_relative_to(root) and p.is_file()


def plan(mod_dir):
    """What the blocks ask to be cooked: {"skeletons": {name: file}, "clips": {(skeleton, file): {key: entry}}, "graphs": [step], "models": [step],
    "physics": [{"model": file, "from": block, "physics": {"cloth": [...], "chains": [...]}}]}.
    Paths are the mod's. Raises BlocksError for what can't be cooked as written."""
    npc_bodies(mod_dir)  # refuse malformed authored motion specs before either cook writes outputs
    blocks = read(mod_dir)
    skeletons, clips, graphs, models = {}, {}, [], []
    physics, physics_of = [], {}
    retargets, wanted = {}, []

    def skeleton_of(where, file, b):
        sk = b.get("skeleton") or stem(file)
        if skeletons.get(sk, file) != file:
            raise BlocksError(f"{where}: skeleton {sk!r} is {skeletons[sk]} already; name this one's \"skeleton\"")
        skeletons[sk] = file
        return sk

    bodies = []
    for fname, name, kind, b in blocks:
        if kind != "body" or not isinstance(b.get("model"), str):
            continue
        where = f"blocks/{fname}: body {name}"
        if not _in_mod(mod_dir, b["model"]):
            continue  # Kiln.Blocks names the missing file
        sk = skeleton_of(where, b["model"], b)
        models.append({"tool": "modelcook", "op": "model", "gltf": b["model"], "skeleton": sk, "name": model_name(b["model"]),
                       "stem": model_name(b["model"]).replace("/", "_"), "materials": "gltf", "mode": "new", "comment": f"{where}"})
        if b.get("cloth") or b.get("chains"):
            file = Path(b["model"]).as_posix()
            if file in physics_of:
                raise BlocksError(f"{where}: its model {file} has cloth and chains from {physics_of[file]} already: declare a model's cloth and chains in one block")
            physics_of[file] = where
            physics.append({"model": file, "from": where, "physics": {"cloth": b.get("cloth") or [], "chains": b.get("chains") or []}})
        bodies.append((name, sk, {_slot(s): j for s, j in (b.get("slots") or {}).items()}))

    for fname, name, kind, b in blocks:
        if kind != "animation" or not isinstance(b.get("clips"), str):
            continue
        where = f"blocks/{fname}: animation {name}"
        if not _in_mod(mod_dir, b["clips"]):
            continue
        file = b["clips"]
        if b.get("retarget") is not None and b.get("retarget") is not False:
            # clips authored on a rig of their own, played on a skeleton a body (or another animation) of the mod makes
            if not b.get("skeleton"):
                raise BlocksError(f"{where}: retarget: say which skeleton the clips play on (\"skeleton\": a body's skeleton, its model file's name)")
            sk = b["skeleton"]
            cfg = b["retarget"] if isinstance(b["retarget"], dict) else {}
            if retargets.get((sk, file), cfg) != cfg:
                raise BlocksError(f"{where}: retarget: {file} is retargeted onto {sk} by another animation already, with other settings")
            retargets[(sk, file)] = cfg
            wanted.append((where, sk))
        else:
            sk = skeleton_of(where, file, b)
        own = clips.setdefault((sk, file), {})

        def source(role, v, loop_default):
            c = clip_spec(v, f"{where}, {role}")
            st = {}
            if "blend" in c:
                bl = c["blend"]
                keys = {}
                for at, anim in (bl.get("clips") or {}).items():
                    k, e = clip_key(sk, anim, loop_default, c)
                    own[k] = e
                    keys[str(at)] = k
                st["blend"] = {"input": bl.get("input", "moveDirection"), "clips": keys}
                for k2 in ("circular", "smoothing"):
                    if k2 in bl:
                        st["blend"][k2] = bl[k2]
            else:
                k, e = clip_key(sk, c["clip"], loop_default, c)
                if own.get(k, e) != e:
                    raise BlocksError(f"{where}, {role}: clip {c['clip']} is cooked twice with different events")
                own[k] = e
                st["clip"] = k
            for k2 in ("speed", "blendIn"):
                if k2 in c:
                    st[k2] = c[k2]
            if "loop" in c:
                st["loop"] = bool(c["loop"])
            return st, c

        states = {}
        for field, engine in ENGINE_FIELDS.items():
            if field in b and b[field] is not None:
                st, c = source(field, b[field], engine in LOOPED)
                st = dict({"engine": engine}, **st)
                if c.get("then"):
                    st["then"] = c["then"]
                states[engine] = st
        for sname, v in (b.get("states") or {}).items():
            if sname in ENGINE_FIELDS.values():
                raise BlocksError(f"{where}, states.{sname}: an engine state is a field of its own: \"{sname[0].lower() + sname[1:]}\"")
            st, c = source(f"states.{sname}", v, False)
            if c.get("then"):
                st["then"] = c["then"]
            states[sname] = st
        if "Idle" not in states:
            raise BlocksError(f"{where}: idle: every graph needs the engine's Idle")
        # the engine asks for these on any creature that stands in a fight or loses its footing: without its own clip, it stands as in Idle
        for engine in DERIVED:
            if engine not in states:
                states[engine] = dict(states["Idle"], engine=engine)
        # What the base creature's hit, stun and death sequences play becomes these (Kiln.Blocks makes those sequences play them). Stagger: the
        # block's, else its hit clip once over the whole body, else Idle once. Stun as Stagger; Dead: Idle, held.
        if "Stagger" in (b.get("states") or {}):
            raise BlocksError(f"{where}, states.Stagger: Stagger is a field of its own: \"stagger\"")
        idle = {k: v for k, v in states["Idle"].items() if k != "engine"}
        if b.get("stagger") is not None:
            st, _ = source("stagger", b["stagger"], False)
        elif b.get("hit") is not None:
            hc = clip_spec(b["hit"], f"{where}, hit")
            st, _ = source("hit", {k: hc[k] for k in ("clip", "blend", "speed") if k in hc}, False)
        else:
            st = dict(idle, loop=False)
        states["Stagger"] = st
        if "Stun" not in states:
            states["Stun"] = dict(st, engine="Stun")
        if "Dead" not in states:
            states["Dead"] = dict(idle, engine="Dead", loop=False)
        layers = []
        if b.get("hit") is not None:
            st, c = source("hit", b["hit"], False)
            lay = dict({"name": "hit", "overlay": "hit", "mode": "override" if not c.get("reference") else "additive"}, **st)
            if c.get("reference"):
                lay["reference"], _ = clip_key(sk, c["reference"], False, {})
                own[lay["reference"]] = {"animation": c["reference"], "loop": False}
            if c.get("bones"):
                lay["bones"] = c["bones"]
            layers.append(lay)
        for i, ly in enumerate(b.get("layers") or []):
            st, c = source(f"layers[{i}]", ly, False)
            lay = dict({"name": ly.get("name") or f"layer{i}", "mode": ly.get("mode") or ("additive" if ly.get("reference") else "override")}, **st)
            for k2 in ("overlay", "bones", "weight", "fadeIn", "fadeOut"):
                if ly.get(k2) is not None:
                    lay[k2] = ly[k2]
            if ly.get("reference"):
                lay["reference"], _ = clip_key(sk, ly["reference"], False, {})
                own[lay["reference"]] = {"animation": ly["reference"], "loop": False}
            layers.append(lay)
        base = {"tool": "animcook", "op": "graph", "name": name, "skeleton": sk, "states": states, "comment": where}
        if layers:
            base["layers"] = layers
        graphs.append(base)
        for bname, bsk, slots in bodies:
            if bsk == sk and slots:
                g = json.loads(json.dumps(base))
                g["name"] = f"{name}.{bname}"
                g["slots"] = slots
                g["comment"] = f"{where}, with the slots of body {bname}"
                graphs.append(g)
    for where, sk in wanted:
        if sk not in skeletons:
            raise BlocksError(f"{where}: retarget: no body or animation of the mod makes skeleton {sk!r} (has {', '.join(skeletons) or 'none'})")
    # a clip's GUID follows from <skeleton>.<animation>: two files can't give one skeleton two animations of one name
    seen = {}
    for (sk, file), entries in clips.items():
        for k in entries:
            if seen.setdefault(k, file) != file:
                raise BlocksError(f"clip {k}: animation {entries[k]['animation']!r} comes from {seen[k]} and {file}, on skeleton {sk}: rename one of them")
    return {"skeletons": skeletons, "clips": clips, "graphs": graphs, "models": models, "retargets": retargets, "physics": physics}


def _plan(mod_dir):
    try:
        return plan(mod_dir)
    except BlocksError as e:
        raise BlocksError(f"blocks: {e}") from e


def steps_from_blocks(mod_dir, recipe_dir, p=None):
    """The steps the mod's blocks ask for, paths relative to the recipe's folder, each with its "tool"."""
    p = p if p is not None else _plan(mod_dir)
    rel = lambda f: os.path.relpath(Path(mod_dir) / f, recipe_dir)  # noqa: E731
    out = []
    for sk, file in p["skeletons"].items():
        out.append({"tool": "animcook", "op": "skeleton", "name": sk, "gltf": rel(file), "comment": "from blocks/*.json"})
    for (sk, file), entries in p["clips"].items():
        st = {"tool": "animcook", "op": "clips", "skeleton": sk, "gltf": rel(file), "clips": entries, "comment": "from blocks/*.json"}
        if (sk, file) in p["retargets"]:
            st["retarget"] = p["retargets"][(sk, file)]
        out.append(st)
    for g in p["graphs"]:
        out.append(g)
    for m in p["models"]:
        m = dict(m, gltf=rel(m["gltf"]))
        out.append(m)
    return out


def mod_dir_of(recipe_path):
    """The mod a recipe belongs to: the nearest folder above it with a kiln.mod.json, else the recipe's folder's parent, as <mod>/assets/cook.json is
    in a project whose build writes the manifest."""
    p = Path(recipe_path).resolve()
    for d in p.parents:
        if (d / "kiln.mod.json").exists():
            return d
    return p.parent.parent if (p.parent.parent / "blocks").is_dir() else None


def add_to(doc, recipe_path):
    """The recipe with the steps of the mod's blocks added; a step of the recipe of the same op and name (a skeleton, graph or model) wins."""
    mod = mod_dir_of(recipe_path)
    if mod is None or not (mod / "blocks").is_dir():
        return doc
    have = {(st.get("op"), st.get("name")) for st in doc.get("steps", [])}
    have |= {("clips", (st.get("skeleton"), st.get("gltf"))) for st in doc.get("steps", []) if st.get("op") == "clips"}
    extra = []
    p = _plan(mod)
    for st in steps_from_blocks(mod, Path(recipe_path).resolve().parent, p):
        key = ("clips", (st["skeleton"], st["gltf"])) if st["op"] == "clips" else (st["op"], st["name"])
        if key not in have:
            extra.append(st)
    if extra or p["physics"]:
        doc = dict(doc, steps=list(doc.get("steps", [])) + extra)
        doc["blocksSources"] = sorted(os.path.relpath(f, Path(recipe_path).resolve().parent) for f in (mod / "blocks").glob("*.json"))
    if p["physics"]:
        # a body's cloth and chains, whichever step cooks its model (one of the recipe's wins over the block's): modelcook reads them with physics.json
        doc["blocksPhysics"] = p["physics"]
    return doc

# Roster bodies use the existing cook, but their resources form one boot transaction, never eager assets.json entries.
MOTION_ROLES = ("sit", "sleep", "inspect", "greet", "talk", "reaction0", "reaction1", "refuse", "idleGesture0", "idleGesture1",
                "interaction", "greetingExtra", "farewell", "dialogOption", "attentionGreeting", "turnLeftSmall", "turnLeftBig",
                "turnRightSmall", "turnRightBig", "jump", "door")
FISHER_INTERACTION = "81ed77f9-4c72-4730-8b05-a5ddee9c7164"


def _manifest_id(root):
    """Read only when a qualified part or material needs the id; manifests use the loader's relaxed JSON dialect."""
    try:
        document = json.loads(_relaxed((Path(root) / "kiln.mod.json").read_text()))
    except (OSError, ValueError) as error:
        raise BlocksError(f"kiln.mod.json: cannot read the mod id: {error}") from error
    if not isinstance(document, dict) or not isinstance(document.get("id"), str) or not document["id"].strip():
        raise BlocksError("kiln.mod.json: a manifest object with a nonempty mod id is required")
    return document["id"]


def npc_bodies(mod_dir):
    """Resolve same-mod NPC parts without exposing game reflection names. Returns source specs in deterministic block order."""
    root = Path(mod_dir)
    if not (root / "blocks").is_dir():
        return {}
    definitions = {s: {} for s in ("npcs", "creatures", "looks", "bodies", "animations", "npcMotions")}
    for f in sorted((root / "blocks").glob("*.json")):
        try:
            doc = json.loads(_relaxed(f.read_text()))
        except ValueError:
            continue  # the block loader diagnoses malformed JSON
        if not isinstance(doc, dict):
            continue
        for section, into in definitions.items():
            group = doc.get(section)
            if not isinstance(group, dict):
                continue  # malformed block sections belong to the loader/linter, not this closure
            for name, b in group.items():
                # The existing block loader lets the later definition replace an earlier one (lint warns).
                # Merely adding body planning must not turn ordinary duplicate body definitions into cook errors.
                into[name] = b
    def part(npc, name, field, section):
        value = npc.get(field)
        if isinstance(value, dict):
            return f"{name}.{field}", value
        if isinstance(value, str):
            local = value
            if ":" in local:
                owner, local = local.split(":", 1)
                if owner != _manifest_id(root):
                    if field == "body" and not npc.get("motions") and not npc.get("animation"):
                        return local, None  # an existing vanilla-body reference is not this cook's closure
                    raise BlocksError(f"npcs.{name}.{field}: cross-mod body dependencies are not supported")
            return local, definitions[section].get(local)
        return None, None

    out = {}
    for name, npc in sorted(definitions["npcs"].items()):
        if not isinstance(npc, dict):
            continue
        bn, body = part(npc, name, "body", "bodies")
        if not isinstance(body, dict) or not body.get("model"):
            continue
        an, animation = part(npc, name, "animation", "animations")
        _, motions = part(npc, name, "motions", "npcMotions")
        if not isinstance(animation, dict) or not animation.get("clips"):
            warnings.warn(f"npcs.{name}.animation: not deferred; keeping the ordinary asset cook. This NPC still needs this mod's cooked animation before its authored body can be defined", stacklevel=2)
            continue
        if not isinstance(motions, dict):
            warnings.warn(f"npcs.{name}.motions: not deferred; keeping the ordinary asset cook. This NPC still needs motion bindings before its authored body can be defined", stacklevel=2)
            continue
        if body.get("template") not in (None, "NPC_Workshop_Fisher_Trader") or body.get("like") not in (None, "fisher", "villager"):
            raise BlocksError(f"npcs.{name}.body: only the Fisher/Villager chassis is supported")
        _body_geometry(body, f"npcs.{name}.body")
        _motion_shape(motions, f"npcs.{name}.motions")
        sk = body.get("skeleton") or stem(body["model"])
        if sk != (animation.get("skeleton") or stem(animation["clips"])):
            raise BlocksError(f"npcs.{name}.animation: body and animation must use the same skeleton")
        out[name] = {"key": npc.get("was") or name, "body": body, "animation": animation, "motions": motions,
                     "skeletonName": sk, "animationName": an, "bodyName": bn, "graphName": f"{an}.{bn}" if body.get("slots") else an,
                     "modelName": model_name(body["model"])}
    if not out:
        return out

    def local_part(value, section):
        if isinstance(value, dict):
            return value
        if isinstance(value, str):
            if ":" in value:
                owner, value = value.split(":", 1)
                if owner != _manifest_id(root):
                    return None
            return definitions[section].get(value)
        return None

    def shared_rig(block, spec):
        if not isinstance(block, dict):
            return False
        source = block.get("model") or block.get("clips")
        return isinstance(source, str) and ((block.get("skeleton") or stem(source)) == spec["skeletonName"] or
                                           (block.get("model") and model_name(source) == spec["modelName"]))

    # A deferred skeleton/model/clip cannot also satisfy an eager graph or creature. Refuse before either tool writes
    # anything rather than silently stripping another block's resources from assets.json. NPCs may share their group.
    owned = {id(spec[field]) for spec in out.values() for field in ("body", "animation")}
    for name, spec in out.items():
        for creature_name, creature in definitions["creatures"].items():
            if not isinstance(creature, dict):
                continue
            if any(shared_rig(local_part(creature.get(field), section), spec)
                   for field, section in (("body", "bodies"), ("animation", "animations"))):
                raise BlocksError(f"npcs.{name}.body: this rig is also used by creature {creature_name}: give the roster body its own file/rig")
        for look_name, look in definitions["looks"].items():
            if isinstance(look, dict) and isinstance(look.get("model"), str) and model_name(look["model"]) == spec["modelName"]:
                raise BlocksError(f"npcs.{name}.body: this model is also used by look {look_name}: give the roster body its own file/rig")
        for section, kind in (("bodies", "body"), ("animations", "animation")):
            for block_name, block in definitions[section].items():
                if id(block) not in owned and shared_rig(block, spec):
                    raise BlocksError(f"npcs.{name}.body: this rig is also used by {kind} {block_name}: give the roster body its own file/rig")
    return out


def _body_geometry(body, where, box=None):
    import math
    if body.get("scale", 1) != 1:
        raise BlocksError(f"{where}.scale: an authored roster body keeps scale 1")
    if body.get("colliders"):
        raise BlocksError(f"{where}.colliders: use Size, not custom roster colliders")
    size = body.get("size")
    if not isinstance(size, dict):
        raise BlocksError(f"{where}.size: an explicit radius and height are required")
    for key, lo, hi in (("radius", .05, 4), ("height", .1, 8)):
        value = size.get(key)
        if type(value) not in (float, int) or not math.isfinite(value) or not lo <= value <= hi:
            raise BlocksError(f"{where}.size.{key}: a finite value in [{lo}, {hi}] metres")
    slots = {_slot(k): v for k, v in (body.get("slots") or {}).items()}
    for slot in ("Root", "Head", "HandR"):
        if not isinstance(slots.get(slot), str) or not slots[slot].strip():
            raise BlocksError(f"{where}.slots[{slot}]: the authored rig needs this joint")
    if box is not None:
        try:
            mn = [box["min"][k] for k in "xyz"]
            mx = [box["max"][k] for k in "xyz"]
            valid = all(math.isfinite(v) for v in mn + mx) and all(a <= b for a, b in zip(mn, mx))
            valid = valid and abs(mn[1]) <= .050001 and mx[1] <= size["height"] + .000001
            valid = valid and all(abs(v) <= size["radius"] + .000001 for v in (mn[0], mx[0], mn[2], mx[2]))
        except (KeyError, TypeError, ValueError):
            valid = False
        if not valid:
            raise BlocksError(f"{where}.size: model bounds must fit the radius-height box with its floor within 0.05 m of Y0")


def _motion_shape(motions, where):
    import uuid
    fallback = motions.get("optionalFallback")
    if fallback is not None and (not isinstance(fallback, str) or not fallback.strip()):
        raise BlocksError(f"{where}.optionalFallback: a graph state name")
    bindings = motions.get("bindings", [])
    if not isinstance(bindings, list):
        raise BlocksError(f"{where}.bindings: a list")
    seen = set()
    for i, b in enumerate(bindings):
        at = f"{where}.bindings[{i}]"
        if not isinstance(b, dict) or set(b) - {"role", "variant", "state", "interactionKey"}:
            raise BlocksError(f"{at}: a binding with role, variant, state and optional interactionKey only")
        role, variant = b.get("role"), b.get("variant")
        if role not in MOTION_ROLES:
            raise BlocksError(f"{at}.role: a known NPC motion role")
        if type(variant) is not int or not 0 <= variant < (3 if role in ("sit", "sleep", "door") else 1):
            raise BlocksError(f"{at}.variant: not a variant of {role}")
        if (role, variant) in seen:
            raise BlocksError(f"{at}: duplicate {role} variant {variant}")
        seen.add((role, variant))
        if not isinstance(b.get("state"), str) or not b["state"].strip():
            raise BlocksError(f"{at}.state: a graph state name")
        supplied_key = b.get("interactionKey")
        key = FISHER_INTERACTION if role == "interaction" and supplied_key is None else None
        if supplied_key is not None:
            try:
                key = str(uuid.UUID(supplied_key))
            except (ValueError, TypeError, AttributeError):
                key = None
        if (role == "interaction" and key != FISHER_INTERACTION) or (role != "interaction" and b.get("interactionKey") is not None):
            raise BlocksError(f"{at}.interactionKey: only Interaction takes the supported nonzero Fisher interaction key")
    for role in MOTION_ROLES:
        for v in range(3 if role in ("sit", "sleep", "door") else 1):
            if (role, v) not in seen and (role in ("sit", "sleep", "inspect", "jump", "door") or not motions.get("optionalFallback")):
                raise BlocksError(f"{where}.bindings: missing {role} variant {v}; required motions cannot fall back, optional motions need optionalFallback")
    if set(motions) - {"bindings", "optionalFallback"}:
        raise BlocksError(f"{where}: unknown motion field")


def body_step(recipe_path, step, specs=None):
    """Select a private NPC closure; never take a shared eager model/graph's rig out of assets.json."""
    if specs is None:
        specs = npc_bodies(mod_dir_of(recipe_path) or Path(recipe_path).parent.parent)
    op = step.get("op")
    owners = [(name, spec) for name, spec in specs.items() if step.get("skeleton") == spec["skeletonName"]]
    if op in ("model", "graph") and owners:
        field = "modelName" if op == "model" else "graphName"
        names = {spec[field] for _, spec in owners}
        if op == "graph":
            names.update(spec["animationName"] for _, spec in owners if "animationName" in spec)  # unbound private base graphs
            # The existing planner also emits the slot-bound cross-product of animations and bodies on one rig.
            names.update(f"{animation['animationName']}.{body['bodyName']}" for _, animation in owners for _, body in owners
                         if "animationName" in animation and "bodyName" in body)
        if step.get("name") not in names:
            raise BlocksError(f"npcs.{owners[0][0]}.body: this rig is also used by {op} {step.get('name', '<unnamed>')}: give the roster body its own file/rig")
        return True
    return any((op == "model" and step.get("name") == spec["modelName"]) or
               (op == "skeleton" and step.get("name") == spec["skeletonName"]) or
               (op == "clips" and step.get("skeleton") == spec["skeletonName"])
               for spec in specs.values())


def _body_bindings(spec, graph, clips, where):
    import math
    motion, anim = spec["motions"], spec["animation"]
    states = graph.get("states", {})

    def target(name, role, variant, fallback, path):
        state = states.get(name)
        if state is None:
            # Engine state names are case insensitive in AnimState; custom names are not.
            canonical = next((engine for engine in ENGINE_FIELDS.values() if engine.lower() == name.lower()), name)
            state = states.get(canonical)
        else:
            canonical = name
        if not state or not state.get("guid"):
            raise BlocksError(f"{path}: the cooked graph has no GUID-bearing state {name!r}")
        engine_field = next((f for f, engine in ENGINE_FIELDS.items() if engine == canonical), None)
        value = anim.get(engine_field) if engine_field else (anim.get("states") or {}).get(canonical)
        if value is None and canonical in ("IdleCombat", "Fall", "Dead", "Stagger", "Stun"):
            value = anim.get("stagger") or anim.get("hit") if canonical in ("Stagger", "Stun") else None
            value = value or anim.get("idle")
        c = clip_spec(value, path)
        if "blend" in c:
            raise BlocksError(f"{path}: a motion needs one clip, not a blend")
        speed = c.get("speed", 1)
        if not isinstance(speed, (int, float)) or not math.isfinite(speed) or speed <= 0:
            raise BlocksError(f"{path}: finite positive speed required")
        key, _ = clip_key(spec["skeletonName"], c["clip"], canonical in LOOPED, c)
        cooked = (clips or {}).get("clips", {}).get(key)
        if not cooked or cooked.get("skeleton") != graph.get("skeleton"):
            raise BlocksError(f"{path}: missing clip {key!r} on the body's skeleton; cook again")
        seconds = cooked.get("duration", 0) / speed
        if not math.isfinite(seconds) or seconds <= 0 or seconds * 1e9 >= 2**63:
            raise BlocksError(f"{path}: finite positive cooked duration required")
        ns = math.floor(seconds * 1e9 + 0.5)
        if ns <= 0:
            raise BlocksError(f"{path}: duration rounds to zero nanoseconds")
        loop = c.get("loop", canonical in LOOPED)
        if not fallback and role in ("sit", "sleep") and bool(loop) != (variant == 1):
            raise BlocksError(f"{path}: Sit/Sleep entry and exit must not loop; their middle variant must loop")
        return {"state": state["guid"], "durationNs": ns, "loop": bool(loop) and not fallback, "clamp": fallback}

    fallback = motion.get("optionalFallback")
    if fallback:
        target(fallback, "greet", 0, True, f"{where}.optionalFallback")
    out = []
    for role in MOTION_ROLES:
        for v in range(3 if role in ("sit", "sleep", "door") else 1):
            b = next((b for b in motion.get("bindings", []) if b["role"] == role and b["variant"] == v), None)
            row = target(b["state"] if b else fallback, role, v, b is None, f"{where}.{role}[{v}].state")
            row.update(role=role, variant=v)
            if role == "interaction":
                row["interactionKey"] = FISHER_INTERACTION
            out.append(row)
    return out


def body_manifest(recipe_path, entries, update, selected=None, producer="modelcook", specs=None, server=False):
    """Split the current cook's entries, merge producer-owned chunks, hash every owned file. No game resource is published here.

    This is a machine-local cache under assets/cooked, never an identity-counted authored file. Output hashes prove only
    local bytes; sourceHash is derived from authored specs and sources for cross-machine agreement. A partial two-tool cook,
    server-skipped renderer output, or unlowered material blocks cannot become Ready. Paths remain mod-relative.
    Returns ordinary entries and missing/mismatched deferred entries for the cook report.
    """
    import hashlib
    from urllib.parse import unquote
    from . import cookpaths as CP
    root = CP.mod_root(recipe_path)  # same lexical root as every checked path, including linked Steam libraries
    if specs is None:
        specs = npc_bodies(root)
    if not specs and not (root / "assets/cooked/npc-bodies.v1.json").exists():
        return entries, [], []
    def object_value(value, where):
        if not isinstance(value, dict):
            raise BlocksError(f"{where}: an object is required")
        return value

    def object_rows(value, where):
        if not isinstance(value, list):
            raise BlocksError(f"{where}: an array of objects is required")
        return [object_value(row, f"{where}[{i}]") for i, row in enumerate(value)]

    try:
        path = CP.npc_manifest(recipe_path)
        old = object_value(json.loads(path.read_text()) if path.exists() else {"format": 1, "entries": [], "bodies": {}}, "npc-bodies.v1")
        old_entries = object_rows(old.get("entries", []), "npc-bodies.v1.entries")
        sources = {}
        for file in sorted({s[field][key] for s in specs.values() for field, key in (("body", "model"), ("animation", "clips"))}):
            raw = CP.source(recipe_path, file).read_bytes()
            sources[file] = hashlib.sha256(raw).hexdigest()
            if Path(file).suffix.lower() == ".glb":
                if len(raw) < 20 or raw[:4] != b"glTF" or raw[16:20] != b"JSON":
                    raise BlocksError(f"{file}: an NPC body source must be a valid GLB")
                document = json.loads(raw[20:20 + int.from_bytes(raw[12:16], "little")].rstrip(b"\0 \t\r\n"))
            else:
                document = json.loads(raw)
            document = object_value(document, file)
            # An external glTF buffer/image can change without changing the glTF JSON. Both producers must then
            # rebuild their part; hashing only the container would let one reuse an obsolete model or rig.
            for section in ("buffers", "images"):
                for entry in object_rows(document.get(section, []), f"{file}.{section}"):
                    uri = entry.get("uri")
                    if isinstance(uri, str) and not uri.startswith("data:"):
                        # URI parents are relative to the glTF, not forbidden mod-relative source syntax. Check the
                        # raw decoded walk first (including link/..), then pass its contained spelling to source().
                        parts, _ = CP._under(root, Path(root) / Path(file).parent / unquote(uri))
                        if parts is None:
                            raise CP.Refused(f"{file}: {uri!r}: an NPC dependency must belong to this mod")
                        relative = Path(*parts).as_posix()
                        sources[relative] = hashlib.sha256(CP.source(recipe_path, relative).read_bytes()).hexdigest()
        source_hash = hashlib.sha256(json.dumps({"specs": specs, "sources": sources}, sort_keys=True,
                                                separators=(",", ":"), ensure_ascii=False).encode()).hexdigest()
        selected = set(selected) if selected is not None else {e["file"] for e in entries} if specs else set()
        normal, deferred = [], []
        for e in entries:
            if e["file"] not in selected:
                normal.append(e)
                continue
            if e.get("mode", "new") != "new":
                raise BlocksError("an NPC body closure may contain new owned resources only, never a vanilla override")
            row = dict(e, producer=producer, sourceHash=source_hash)
            file = CP.cooked(recipe_path, e["file"], "an NPC body's asset")
            row["file"] = file.relative_to(root).as_posix()
            # Model blob patches use the same path base as the manifest.
            if row.get("patch"):
                row["patch"] = [dict(p, blob=CP.cooked(recipe_path, p["blob"], "an NPC body's blob").relative_to(root).as_posix()) for p in row["patch"]]
            if server and producer == "modelcook":
                row["skippedOnServer"] = True
                row["sha256"] = None  # no renderer output was made here; never borrow an author's expected bytes
            elif file.is_file():
                row["sha256"] = hashlib.sha256(file.read_bytes()).hexdigest()
            else:
                raise BlocksError(f"{row['file']}: {producer} did not produce this body resource")
            deferred.append(row)
        wanted = [dict(e) for e in old_entries if e.get("producer") != producer] + deferred if specs else []
        wanted.sort(key=lambda e: (e["file"], e.get("kind", ""), e.get("type", "")))
        # Only the tool that rebuilt a file can establish its expected hash. Validate the other producer's bytes,
        # but never bless corruption by replacing that producer's expected digest (or mutating the old manifest).
        foreign_mismatched = []
        for e in wanted:
            file = CP.cooked(recipe_path, Path(root) / e["file"], "an NPC body's asset")
            if e.get("producer") != producer and not e.get("skippedOnServer") and file.is_file() and hashlib.sha256(file.read_bytes()).hexdigest() != e.get("sha256"):
                foreign_mismatched.append(e)
            # Hashes prove this machine's outputs only. Cross-machine agreement uses authored sourceHash, not these bytes.
        bodies = {}
        recipe = object_value(json.loads(Path(recipe_path).read_text()), str(recipe_path))
        cooked_dir = Path(recipe_path).parent / recipe.get("out", "cooked")
        anim_current = any(e.get("producer") == "animcook" and e.get("sourceHash") == source_hash for e in wanted)
        model_current = any(e.get("producer") == "modelcook" and e.get("sourceHash") == source_hash for e in wanted)

        def metadata(file):
            checked = CP.cooked(recipe_path, file, "an NPC body's metadata")
            return object_value(json.loads(checked.read_text()), checked.name) if checked.is_file() else None

        for name, spec in specs.items():
            graph = metadata(cooked_dir / f"{spec['graphName']}.graph.json")
            skipped_model = any(e.get("producer") == "modelcook" and e.get("skippedOnServer") for e in wanted)
            model = None if skipped_model else metadata(cooked_dir / f"{spec['modelName'].replace('/', '_')}.model.json")
            clips = metadata(cooked_dir / "clips.json")
            bindings = _body_bindings(spec, graph, clips, f"npcs.{name}.motions") if graph and clips and anim_current else []
            materials, dependencies = [], {}
            def dependency(file):
                checked = CP.source(recipe_path, str(file))
                rel = checked.relative_to(root).as_posix()
                dependencies[rel] = {"file": rel, "sha256": hashlib.sha256(checked.read_bytes()).hexdigest()}
                return dependencies[rel]
            for file in (cooked_dir / f"{spec['graphName']}.graph.json", cooked_dir / f"{spec['modelName'].replace('/', '_')}.model.json", cooked_dir / "clips.json"):
                if skipped_model and file.name == f"{spec['modelName'].replace('/', '_')}.model.json":
                    continue
                if file.is_file():
                    dependency(file.relative_to(root).as_posix())
            from .graphgen import derive
            for material_file in sorted((Path(root) / "blocks").glob("*.json")):
                document = object_value(json.loads(_relaxed(CP.source(recipe_path, material_file.relative_to(root).as_posix()).read_text())), material_file.name)
                material_blocks = object_value(document.get("materials") or {}, f"{material_file.name}.materials")
                for key, block in material_blocks.items():
                    block = object_value(block, f"{material_file.name}.materials.{key}")
                    if not model or derive(graph["mod"] if graph else _manifest_id(root), "material", block.get("was") or key) not in model.get("materials", []):
                        continue
                    item = dependency(material_file.relative_to(root).as_posix())
                    if item not in materials:
                        materials.append(item)
                    def textures(value):
                        if isinstance(value, dict):
                            for v in value.values():
                                textures(v)
                        elif isinstance(value, str) and value.lower().endswith(".png"):
                            dependency(value)
                    textures(block)
            if model and model_current:
                if not model.get("boundingBox"):
                    raise BlocksError(f"npcs.{name}.body.size: the cooked model has no bounds; cook again")
                _body_geometry(spec["body"], f"npcs.{name}.body", model["boundingBox"])
            if model and graph and anim_current and model_current and model.get("skeleton") != graph.get("skeleton"):
                raise BlocksError(f"npcs.{name}.body.model: model and graph skeletons differ")
            # G2: material JSON hashes are dependencies, NOT evidence of lowered material resource bytes.
            bodies[name] = {"key": spec["key"], "sourceHash": source_hash, "spec": spec, "model": model.get("guid") if model else None,
                            "graph": graph.get("guid") if graph else None, "skeleton": graph.get("skeleton") if graph else None,
                            "bindings": bindings, "materialBlocks": materials, "files": [dependencies[k] for k in sorted(dependencies)],
                            "complete": bool(model and graph and bindings and anim_current and model_current and wanted and
                                             all(e.get("sha256") and e.get("sourceHash") == source_hash for e in wanted) and not materials)}
        doc = {"format": 1, "sourceHash": source_hash, "entries": wanted, "bodies": bodies}
        have = {(e["kind"], e["file"]): e for e in old_entries}
        missing = [e for e in deferred if (e["kind"], e["file"]) not in have]
        mismatch = [e for e in deferred if (e["kind"], e["file"]) in have and have[(e["kind"], e["file"])] != e]
        if update:
            if doc != old and not foreign_mismatched:
                CP.write(path, (json.dumps(doc, indent=2, sort_keys=True) + "\n").encode())
            if not foreign_mismatched:
                missing, mismatch = [], []
        return normal, missing, mismatch + foreign_mismatched
    except (CP.Refused, OSError, ValueError) as e:
        raise BlocksError(f"npc-bodies.v1: {e}") from e


def body_receipt(recipe_path, game):
    """One receipt family for the shared local cache, so the second producer cannot stale the first one's receipt."""
    from . import cookpaths as CP
    try:
        path = CP.npc_manifest(recipe_path)
        if path.is_file():
            # The cache itself is the stable recipe key even when different cook.json files contribute to the same mod.
            return CP.receipt(game, path, "npc-bodies", {path: path.read_bytes()})
        return None
    except CP.Refused as error:
        raise BlocksError(f"npc-bodies.v1 receipt: {error}") from error
