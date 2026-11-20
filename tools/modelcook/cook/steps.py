# SPDX-License-Identifier: MIT
"""The cook steps behind the CLI and cook.json recipes. Each step reads vanilla inputs from the installed game (read
only) and returns the bytes it produced plus the kiln.assets entries that serve them (file names relative to the
mod's assets/ folder)."""
import hashlib
import json
import re
import struct
from pathlib import Path

from kfc.archive import ResourceKey
from kfc.blob import Decoder
from kfc.blobwrite import Encoder
from kfc.util import guid_bytes, guid_text

from . import chains as CH, fabric as FB, features as FT, gltf, material as MB, physics as PH, rendermodel as RM, skin as SK
from .validate import validate


class StepError(Exception):
    pass


# The one skeleton of the player, for both bodies, and the animation graph that says which of its joints a slot is.
PLAYER_SKELETON = "87b29df6-6518-43ef-9298-e3f85a6204b9"
GRAPH_TYPE = "keen::anim_graph::runtime_graph::AnimationGraphResource2_0"


def _norm_guid(g):
    return guid_text(guid_bytes(g))


def _material_entry(k, t, guid, base_materials, own=None):
    """A model's RenderModelMaterial: a MaterialBlock of this mod by name (own: {"blocks", "mod"}), or a game material by GUID."""
    if not MB.GUID.match(str(guid)):
        got = MB.entry(guid, own["blocks"], own["mod"]) if own else None
        if got is None:
            raise StepError(f"material {guid!r} is neither a GUID nor a block in this mod's blocks/*.json \"materials\"")
        return got
    guid = _norm_guid(guid)
    for m in base_materials:
        if m["material"] == guid:
            return dict(m)
    th = t.get("keen::RenderMaterialResource")["hash"]
    key = ResourceKey(guid, th, 0)
    if not k.has(key):
        raise StepError(f"material {guid} is not a keen::RenderMaterialResource in {k.path}")
    v = Decoder(t).decode(th, k.read(key))
    return {"material": guid, "renderEffectId": v["effectId"], "materialFlags": v["flags"]}


def game_target(k, t, guid):
    """A skeleton of the installed game, for binding a mesh to it."""
    ht = t.get("keen::ModelHierarchyResource")
    key = ResourceKey(guid, ht["hash"], 0)
    if not k.has(key):
        raise StepError(f"skeleton {guid} is not a keen::ModelHierarchyResource of this game")
    return SK.Target.from_game(Decoder(t, max_array=1 << 26).decode(ht, k.read(key)), key.guid)


def _graph_slots(value):
    """(skeleton, slotBoneIndexMapping) of a decoded AnimationGraphResource2_0."""
    for v in (value, *[x for x in value.values() if isinstance(x, dict)]):
        if "hierarchy" in v:
            return v["hierarchy"], v.get("slotBoneIndexMapping")
    return None, None


def slot_joints(k, t, skeleton):
    """{keen::AttachmentSlot name: joint} of a skeleton, from the first of the game's animation graphs on it. Slots no graph sets are left out, as is Root."""
    gt = t.get(GRAPH_TYPE)
    if gt is None:
        return {}
    names = [v["name"] for v in t.get("keen::AttachmentSlot")["values"]]
    dec = Decoder(t, max_array=1 << 26)
    for key in k.resources_of_type(gt["hash"]):
        hierarchy, table = _graph_slots(dec.decode(gt["hash"], k.read(key)))
        if hierarchy == skeleton and table:
            return {names[i]: j for i, j in enumerate(table) if j and i < len(names) and names[i] != "Count"}
    return {}


def attach_joint(k, t, target, name):
    """What "attach" says: a slot's name, as the game calls them (any case), or j<index>."""
    if isinstance(name, str) and name[:1] in "jJ" and name[1:].isdigit():
        return int(name[1:])
    slots = slot_joints(k, t, target.guid)
    for slot, joint in slots.items():
        if slot.lower() == str(name).lower():
            return joint
    raise StepError(f"attach: {name!r} is not a slot of skeleton {target.guid}; there are {', '.join(sorted(slots)) or 'none'}, or say j<index>")


def player_reference(k, t, out, body="male"):
    """The player's skeleton as a glTF to weight against, from the installed game, with a body in it to model around unless body is None. Joints are named j<index>, and j<index>_<slot> where a
    slot sits. The body is a mesh node named reference_body, which a cook leaves out, so an author can export the whole scene. Returns what it wrote: {"joints", "named", "body_parts", "vertices"}."""
    from kfc import mesh as M
    h = M.load_hierarchy(k, t, PLAYER_SKELETON)
    names = {i: f"j{i}" for i in range(h["jointCount"])}
    named = {}
    for slot, joint in slot_joints(k, t, PLAYER_SKELETON).items():
        if joint not in named and not slot.startswith("Generic"):
            named[joint] = slot
            names[joint] = f"j{joint}_{slot}"
    tri = M.Tris()
    parts = 0
    if body:
        th = t.get("keen::ItemInfo")["hash"]
        mh = t.get("keen::RenderModel")["hash"]
        dec = Decoder(t)
        female = body == "female"
        want = re.compile(rf"^VC_Body_(Head_{'Female' if female else 'Male'}_01_.*|(?!Head)\w+_01)$")
        for key in k.resources_of_type(th):
            v = dec.decode(th, k.read(key))
            if not want.match(v.get("debugName") or ""):
                continue
            eq = v["equipment"]
            model = eq.get("visualModel")
            if female:
                model = next((m["model"] for m in eq.get("visualModels") or [] if "Female" in m["mask"]), model)
            if model and k.has(ResourceKey(model, mh, 0)):
                tri.submeshes += M.to_rh(M.triangles(M.load_render_model(k, t, model), 0)).submeshes
                parts += 1
        if not parts:
            raise StepError("no body parts found in this game (VC_Body_*): say --body none")
    M.write_gltf(tri, out, name="reference_body", skeleton=M.skeleton_rh(h), joint_names=names)
    return {"joints": h["jointCount"], "named": {names[j]: s for j, s in named.items()}, "body_parts": parts, "vertices": tri.vertex_count}


def global_scale_of(k, t, hierarchy):
    """The value the game's models on a skeleton carry: one per skeleton, in all 249 that have models. The renderer doesn't apply it (docs/re/models.md 5)."""
    th = t.get("keen::RenderModel")
    for key in k.resources_of_type(th["hash"]):
        raw = k.read(key)
        if guid_text(raw[0x80:0x90]) == hierarchy:
            return struct.unpack_from("<f", raw, 0x9C)[0]
    return 1.0


def model_entries(out_guid, mode, fb, fo):
    """The kiln.assets entries of a cooked model: its render data blob and its RenderModel, which names the blob. They follow from the step alone
    (GUID, mode, file names), so a cook that makes no model (a dedicated server's) knows them as a client's cook writes them."""
    return [{"kind": "blob", "file": fo},
            {"kind": "resource", "guid": out_guid, "type": "keen::RenderModel", "part": 0, "file": fb,
             "mode": mode, "patch": [{"offset": RM.RENDER_DATA_OFFSET, "blob": fo}]}]


def model_identity(guid, base_guid, mode):
    """(GUID, mode) of the model a step makes, from its `guid` (None: the base's) and `mode` (None: override when it is the base's GUID, else new)."""
    out_guid = _norm_guid(guid) if guid else _norm_guid(base_guid)
    return out_guid, mode or ("override" if base_guid and out_guid == _norm_guid(base_guid) else "new")


def joint_index(target, name):
    """A skeleton joint by name: as the skeleton names it, or j<index> (j17, j17_Head), as tools/kfc and `kiln skeleton player` write them. None when it has none."""
    j = target.index.get(name)
    if j is None and (m := re.fullmatch(r"j(\d+)(_.*)?", name)):
        j = int(m[1])
    return j if j is not None and 0 <= j < len(target) else None


def _collider_skin(target, joint, where):
    if target is None:
        if joint is not None:
            raise StepError(f"{where}.joint: {joint!r}: the model has no skeleton, so a collider follows no joint: leave \"joint\" out (or null)")
        return {"jointIndices": [0, 0, 0, 0], "jointWeights": [0, 0, 0, 0]}
    if joint is None:
        raise StepError(f"{where}.joint: the model is skinned: name the joint the collider follows")
    j = joint_index(target, joint)
    if j is None:
        raise StepError(f"{where}.joint: {joint!r} is not a joint of skeleton {target.guid} ({target.source}, {len(target)} joints)")
    if j > 255:
        raise StepError(f"{where}.joint: {joint!r} is joint {j}: a collider's skin holds joints below 256")
    return {"jointIndices": [j, 0, 0, 0], "jointWeights": [255, 0, 0, 0]}


def _colliders(physics, target, offset=(0.0, 0.0, 0.0)):
    """The model's keen::RenderModelClothColliderData: every cloth entry's colliders once (bind pose, the game's axes), and per cloth mesh an
    instance naming its own (as the game's models have one per cloth mesh). offset: what the model's vertices moved by (glTF space): a model
    attached to one joint has its file's origin at the joint, and its colliders go with its vertices."""
    ox, oy, oz = offset
    spheres, capsules, where = [], [], {}
    for ci, c in enumerate(physics.cloth):
        for i, x in enumerate(c.colliders):
            where.setdefault(x, f"{c.where}.colliders[{i}]")
    for x in physics.colliders():
        w = where[x]
        if isinstance(x, PH.Sphere):
            spheres.append((x, {"center": FB._v3(FB.to_game((x.x + ox, x.y + oy, x.z + oz))), "radius": x.radius, "skinningData": _collider_skin(target, x.joint, w)}))
        else:
            capsules.append((x, {"center0": FB._v3(FB.to_game((x.a.x + ox, x.a.y + oy, x.a.z + oz))), "radius0": x.a.radius,
                                 "center1": FB._v3(FB.to_game((x.b.x + ox, x.b.y + oy, x.b.z + oz))), "radius1": x.b.radius,
                                 "skinningData": _collider_skin(target, x.joint, w + ".a")}))
    if not spheres and not capsules:
        return dict(RM.NO_COLLIDER)
    s_at = {x: i for i, (x, _) in enumerate(spheres)}
    c_at = {x: i for i, (x, _) in enumerate(capsules)}
    instances = [{"sphereIndices": [s_at[x] for x in c.colliders if x in s_at], "capsuleIndices": [c_at[x] for x in c.colliders if x in c_at]} for c in physics.cloth]
    return {"hasSkinning": target is not None, "ignoreClothCollider": False, "spheres": [r for _, r in spheres], "capsules": [r for _, r in capsules],
            "instances": instances}


def _cloth(t, doc, lod0, physics, target, features, global_scale, swinging=None, offset=(0.0, 0.0, 0.0)):
    """The cloth of a model step: its RenderClothMesh per entry, its colliders, which primitives they are (rendermodel.cloth_block). doc: LOD 0's glTF,
    bound; lod0: its prepared primitives (game space). swinging: {skeleton joint: the chain that swings it} of the same model: a cloth vertex may
    not be weighted to one (the cloth's simulation skin follows the joint's animated pose, not the swing)."""
    where = physics.where
    try:
        FT.require(PH.CLOTH, features, f"{where}.cloth")
    except FT.FeatureOff as e:
        raise StepError(str(e)) from e
    problems = RM.layout_problems(t, "cloth")
    if problems:
        raise StepError(f"{where}.cloth: this game's cloth types differ from the ones this cook writes ({problems[0]}{f'; {len(problems) - 1} more' if len(problems) > 1 else ''}): "
                        "it writes no cloth for this build. The mod's other models cook as before")
    if global_scale not in (None, 1.0):
        raise StepError(f"{where}.cloth: the model's globalScale is {global_scale:g}: cloth needs 1 in this version (the game scales its colliders by it)")
    meshes, primitives = [], {}
    for ci, c in enumerate(physics.cloth):
        at = [pi for pi, p in enumerate(doc["primitives"]) if p.get("mesh") == c.mesh and p.get("primitive") == c.primitive]
        if not at:
            raise StepError(f"{c.where}: {doc['file']} has no mesh {c.mesh} with a primitive {c.primitive} in its scene (reference_* nodes are left out)")
        if len(at) > 1:
            raise StepError(f"{c.where}: mesh {c.mesh} of {doc['file']} is used by {len(at)} nodes: cloth is one primitive in one place, make the copies meshes of their own")
        pi = at[0]
        prim = doc["primitives"][pi]
        if prim.get("morphs"):
            raise StepError(f"{c.where}: the primitive has morph targets ({', '.join(sorted(prim['morphs']))}): cloth with morph targets is not written in this version")
        for v, inf in enumerate((prim.get("influences") or []) if swinging else []):
            for j, _ in inf:
                if j in swinging:
                    raise StepError(f"{c.where}: vertex {v} is weighted to joint {target.names[j]!r}, which swings in {swinging[j]}: the cloth's simulation skin "
                                    "would follow the joint's animated pose, not the swing. Weight the cloth to joints that do not swing")
        try:
            mesh = PH.physical_mesh(c, prim["positions"], prim["indices"], prim.get("influences") if target is not None else None)
            result = FB.cook(mesh.positions, mesh.triangles, mesh.pin, c.mass_kg)
            res = FB.mesh_resource(mesh, result, prim["positions"], lod0[pi]["uvs"], prim["indices"], skin=mesh.influences if target is not None else None)
        except PH.PhysicsError as e:
            raise StepError(str(e)) from e
        except FB.FabricError as e:
            raise StepError(f"{c.where}: {e}") from e
        meshes.append(FB.cloth_mesh(c, res))
        primitives[pi] = ci + 1
    return RM.cloth_block(meshes, primitives, _colliders(physics, target, offset), target is not None)


def _chains(t, physics, target, attach, lods, features, global_scale, mod, animated):
    """The bone chains of a model step (cook/chains.py): its palette and ChainData, for rendermodel.build."""
    where = physics.where
    try:
        FT.require(PH.CHAINS, features, f"{where}.chains")
    except FT.FeatureOff as e:
        raise StepError(str(e)) from e
    problems = RM.layout_problems(t, "chains2")
    if problems:
        raise StepError(f"{where}.chains: this game's chain types differ from the ones this cook writes ({problems[0]}{f'; {len(problems) - 1} more' if len(problems) > 1 else ''}): "
                        "it writes no chains for this build. The mod's other models cook as before")
    if target is None:
        raise StepError(f"{where}.chains: bone chains swing joints of the model's skeleton: the model is not skinned (name its \"skeleton\")")
    if attach is not None:
        raise StepError(f"{where}.chains: the model is attached to one joint (\"attach\"), so no vertex follows a chain's joints")
    if global_scale not in (None, 1.0):
        raise StepError(f"{where}.chains: the model's globalScale is {global_scale:g}: chains need 1 in this version")
    try:
        plan = CH.plan(physics, target, where)
        for j in plan.simulated:
            name = target.names[j]
            if name in (animated or {}):
                c = next(c for c, path, _ in plan.chains if j in path)
                raise StepError(f"{c.where}: joint {name!r} swings in this chain and {animated[name][0]} animates it: a swinging joint carries no "
                                "animation in this version (the game simulates it); take its channels out of the clip")
        smap, index = CH.palette(lods, plan)
        data, report = CH.chain_data(plan, target, index, mod, physics.model)
    except CH.ChainError as e:
        raise StepError(str(e)) from e
    ordinary = smap[:len(smap) - len(plan.simulated)]
    swinging = {j: c.where for c, path, _ in plan.chains for j in path}
    return {"data": data, "smap": smap, "model_index": index, "report": report, "palette": ordinary + plan.simulated, "swinging": swinging}


def model(k, t, gltf_files, base, guid=None, mode=None, name=None, material=None, material_map=None,
          pixel_sizes=None, stem=None, subdir="cooked", target=None, global_scale=None, own_materials=None, attach=None,
          physics=None, features=frozenset(), mod=None, animated=None):
    """glTF (one file per LOD, LOD 0 first) -> RenderModel .bin + renderData .blob. `base` is a game model whose materials are the defaults, or None. With `target`
    (a skin.Target) the model is skinned on that skeleton, and with `attach` too (a slot's name or j<index>) rigid on one of its joints. Morph targets named as keen::BlendShapeFlags
    become blend shapes. own_materials {"blocks", "mod"}: the mod's MaterialBlocks, which material and material_map may name.
    physics: the model's physics.ModelPhysics (cloth: one LOD, unit globalScale, no morph target on a cloth primitive; chains: a skeleton of the
    mod's own); features: the cook features the recipe turns on (cook/features.py), which have to include the ones the physics needs. mod: the
    mod's id (a chain's GUID follows from it); animated: {joint name: [clip, ...]} of the clips cooked for this skeleton, which no swinging
    joint may be in.
    Returns (files {relative name: bytes}, entries, report)."""
    if physics is not None and physics.cloth and len(gltf_files) > 1:
        raise StepError(f"{physics.where}.cloth: the model has {len(gltf_files) - 1} more LOD file(s): a model with cloth has one LOD in this version")
    th = t.get("keen::RenderModel")["hash"]
    base_desc, base_key = {"materials": []}, None
    if base is not None:
        base_key = ResourceKey(base, th, 0)
        if not k.has(base_key):
            raise StepError(f"base {base} is not a keen::RenderModel of this game")
        base_desc = Decoder(t).decode(th, k.read(base_key))
    elif not guid:
        raise StepError("a model step without a base needs a guid")
    out_guid, mode = model_identity(guid, base_key.guid if base_key else None, mode)
    if mode == "override" and not k.has(ResourceKey(out_guid, th, 0)):
        raise StepError(f"mode override: {out_guid} is not a game RenderModel")
    if mode == "new" and k.by_guid(out_guid):
        raise StepError(f"mode new: {out_guid} already names a game resource")
    docs = [gltf.read(f) for f in gltf_files]
    warnings = []
    if attach is not None and target is None:
        raise StepError(f"attach: {attach!r} is a joint of a skeleton: name it with \"skeleton\" in the step")
    joint = attach_joint(k, t, target, attach) if attach is not None else None
    for doc in docs:
        skinned = any("skin" in p for p in doc["primitives"])
        if joint is not None:
            warnings += SK.attach(doc, target, joint)
        elif target is not None:
            warnings += SK.bind(doc, target)
        elif skinned:
            raise StepError(f"{doc['file']} has a skinned mesh: name its skeleton with \"skeleton\" in the step")
    mmap = dict(material_map or {})
    slots, materials = {}, []

    def slot_for(prim):
        key = prim["material_name"] if prim["material_name"] is not None else f"#{prim['material']}"
        if key not in slots:
            g = mmap.get(key) or material or (base_desc["materials"] and base_desc["materials"][min(len(slots), len(base_desc["materials"]) - 1)]["material"])
            if not g:
                raise StepError(f"glTF material {key!r} has no game material: give \"material\", \"materialMap\" or a \"base\" model")
            slots[key] = len(materials)
            materials.append(_material_entry(k, t, g, base_desc["materials"], own_materials))
        return slots[key]

    lods = []
    for doc in docs:
        lod = []
        for prim in doc["primitives"]:
            p = RM.prepare(prim)
            p["material_slot"] = slot_for(prim)
            lod.append(p)
        lods.append(lod)
    SK.check_limits(lods, pixel_sizes, materials)
    name = name or Path(gltf_files[0]).stem
    morphs = []
    if target is not None:
        morphs, more = SK.morph_names(lods)
        warnings += more
    elif any(p.get("morphs") for lod in lods for p in lod):
        warnings.append("morph targets are left out: the game raises them on worn models only, which name a \"skeleton\"")
    skin = None
    if target is not None:
        bad_effect = [m["material"] for m in materials if m["renderEffectId"] != "Pbr"]
        if bad_effect:
            raise StepError(f"material {bad_effect[0]} has effect {next(m['renderEffectId'] for m in materials if m['material'] == bad_effect[0])}: every skinned model of the game uses Pbr")
        if global_scale is None:
            global_scale = base_desc["globalScale"] if base_desc.get("hierarchy") == target.guid else global_scale_of(k, t, target.guid) if k.by_guid(target.guid) else 1.0
        skin = {"hierarchy": target.guid, "hierarchyJointCount": len(target), "globalScale": global_scale}
    chains = _chains(t, physics, target, attach, lods, features, global_scale, mod, animated) if physics is not None and physics.chains else None
    attach_offset = (-target.bind[joint][0][3], target.bind[joint][1][3], target.bind[joint][2][3]) if joint is not None else (0.0, 0.0, 0.0)  # as skin.attach
    cloth = _cloth(t, docs[0], lods[0], physics, target, features, global_scale, chains["swinging"] if chains else None, attach_offset) \
        if physics is not None and physics.cloth else None
    desc, blob = RM.build(lods, materials, base_desc, name, pixel_sizes, skin=skin, morphs=morphs, cloth=cloth, chains=chains)
    data = RM.encode(t, desc)
    stem = stem or name
    fb, fo = f"{subdir}/{stem}.bin", f"{subdir}/{stem}.blob"
    own_guids = {m["material"] for m in materials if not k.has(ResourceKey(m["material"], t.get("keen::RenderMaterialResource")["hash"], 0))} if own_materials else ()
    report = validate(k, t, data, blob, against=base_key.guid if base_key else None, gltf_docs=docs, target=target, own_materials=own_guids,
                      palette=chains["palette"] if chains else None)
    failed = sorted(c for c, ok in report["checks"].items() if c.startswith(("cloth_", "chain_")) and not ok)
    if failed:  # physics the game would read wrong is not written: no file of this model leaves the step
        raise StepError(f"{(physics.where + ': ') if physics is not None else ''}the cooked model's cloth or chains did not read back as written "
                        f"({', '.join(failed)}): nothing of it is written. This is a bug of the cook: please report it with the model")
    report.update({"op": "model", "guid": out_guid, "mode": mode, "base": base_key.guid if base_key else None, "bin": fb, "blob": fo,
                   "bin_size": len(data), "blob_size": len(blob), "content_hash": desc["renderData"]["hash"],
                   "materials": materials, "warnings": warnings})
    if cloth:
        report["cloth"] = {"meshes": [{"particles": m["data"]["fabric"]["numParticles"], "renderVertices": len(m["data"]["renderPositions"]),
                                       "phases": len(m["data"]["fabric"]["sets"]), "pinned": len(m["data"]["skinnedVertexIndices"])} for m in cloth["data"]["meshes"]],
                           "spheres": len(cloth["collider"]["spheres"]), "capsules": len(cloth["collider"]["capsules"]), "skinned": cloth["data"]["hasSkinning"]}
    if chains:
        report["chains"] = chains["report"]
    entries = model_entries(out_guid, mode, fb, fo)
    files = {fb: data, fo: blob}
    if target is not None:
        side = {"format": 1, "tool": "modelcook", "guid": out_guid, "name": name, "skeleton": target.guid, "joints": desc["skinningMap"], "lods": len(desc["lods"]),
                "materials": [m["material"] for m in materials], "boundingBox": desc["boundingBox"], "globalScale": desc["globalScale"], "morphs": morphs}
        if attach is not None:
            side["attach"] = str(attach)
        if physics is not None:  # what Kiln.Blocks checks a body's cloth and chains against (physics.spec_hash): a body asks for what was cooked, or is refused
            side["physics"] = {"spec": PH.spec_hash(physics), "cloth": len(physics.cloth), "chains": len(physics.chains)}
        files[f"{subdir}/{stem}.model.json"] = json.dumps(side, indent=1).encode() + b"\n"
    return files, entries, report


def visual(k, t, base, model_guid, guid, name=None, stem=None, subdir="cooked"):
    """A new visual template: a copy of a vanilla keen::ecs::TemplateResource (an item's equipment.visualEntity)
    whose single ecs::ModelResource points at `model_guid`."""
    th = t.get("keen::ecs::TemplateResource")["hash"]
    key = ResourceKey(base, th, 0)
    if not k.has(key):
        raise StepError(f"{base} is not a keen::ecs::TemplateResource")
    guid = _norm_guid(guid)
    if k.by_guid(guid):
        raise StepError(f"{guid} already names a game resource: pick a new GUID")
    dec = Decoder(t)
    v = dec.decode(th, k.read(key))
    swapped = 0
    for c in v["components"]:
        if c and c.get("$type") == "keen::ecs::ModelResource":
            c["value"]["model"] = _norm_guid(model_guid)
            swapped += 1
    if swapped != 1:
        raise StepError(f"template {key.guid} has {swapped} ModelResource components (expected 1)")
    v["name"] = name or v["name"] + "_Kiln"
    data = Encoder(t).encode(th, v)
    ok = Encoder(t).encode(th, dec.decode(th, data)) == data
    stem = stem or v["name"]
    fb = f"{subdir}/{stem}.bin"
    report = {"op": "visual", "guid": guid, "name": v["name"], "model": _norm_guid(model_guid), "base": key.guid,
              "components": [c["$type"] for c in v["components"] if c], "bin": fb, "bin_size": len(data),
              "checks": {"reencodes_byte_exact": ok}, "ok": ok}
    entries = [{"kind": "resource", "guid": guid, "type": "keen::ecs::TemplateResource", "part": 0, "file": fb,
                "mode": "new"}]
    return {fb: data}, entries, report


def gltf_blocks(gltf_file, mod_dir, stem, out):
    """The glTF's own materials as MaterialBlocks: the file of blocks and any PNGs embedded in it, for kiln.vfx to build at boot.
    -> ({glTF material: block name}, {block name: block}, files {path relative to the mod: bytes}, warnings)."""
    try:
        names, blocks, images, warnings = MB.from_gltf(gltf_file, mod_dir, stem, out)
    except MB.MaterialError as e:
        raise StepError(str(e)) from e
    if not blocks:
        raise StepError(f"{gltf_file}: it has no materials to make blocks from")
    files = dict(images)
    files[f"blocks/{stem}.materials.json"] = MB.blocks_file(blocks, Path(gltf_file).name).encode()
    return names, blocks, files, warnings


def sha256(b):
    return hashlib.sha256(b).hexdigest()
