#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes a model's cloth and chains into assets/physics.json (physics format 1, tools/modelcook/cook/physics.py) from what you painted in
Blender. A convenience: physics.json is the contract, and you can write or edit it by hand.

In Blender (5.x; it exports the glTF too, so the ids match that file):

    blender -b scene.blend --python tools/modelcook/blender/kiln_physics_export.py -- \\
        --gltf MOD/models/cape.gltf --key models/cape.gltf --object Cape --pin-group pin [--mass 0.8] \\
        [--chain pendant_0:pendant_2 ...] [--out MOD/assets/physics.json]

  --object is the cloth's mesh object (more than one: say it again), --pin-group its vertex group whose weights are the pins: 1 holds a vertex
  on the model, 0 lets it hang (a vertex outside the group is 0). The scene is exported to --gltf with Blender's glTF exporter (modifiers
  applied, the group's weights carried as the attribute _KILN_PIN), then the exported file is read back: every primitive with that attribute
  becomes a cloth entry, its seam copies welded by id. --chain names a chain's root and tip bone.

Without Blender, on a glTF that already has the attribute (exported with Attributes on, from a float point attribute named _KILN_PIN):

    python3 tools/modelcook/blender/kiln_physics_export.py --gltf MOD/models/cape.gltf --key models/cape.gltf [--mass 0.8] [--chain ...] \\
        [--out MOD/assets/physics.json]

The model's entry in --out is replaced; other models in it stay. The result is read back with the cook's own reader before it is written.
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from cook import gltf as G, physics as P  # noqa: E402

ATTRIBUTE = "_KILN_PIN"


class ExportError(Exception):
    pass


def _skin(doc, buffers, at, n):
    """Per vertex the (joint, weight) pairs of every JOINTS_n/WEIGHTS_n set, weights above 0, in 255ths: what decides whether two copies are one particle."""
    out = [[] for _ in range(n)]
    k = 0
    while f"JOINTS_{k}" in at and f"WEIGHTS_{k}" in at:
        for o, js, ws in zip(out, G._accessor(doc, buffers, at[f"JOINTS_{k}"]), G._accessor(doc, buffers, at[f"WEIGHTS_{k}"])):
            o.extend((int(j), round(float(w) * 255)) for j, w in zip(js, ws) if w > 0)
        k += 1
    return [tuple(sorted(s)) for s in out]


def cloth_entries(path, attribute=ATTRIBUTE, mass=1.0):
    """The cloth entries of a glTF: one per primitive with `attribute` (its pin per vertex). Copies at the same position with the same pin and skin
    (what a UV or normal seam makes) get the same particle id."""
    doc, buffers = G._load(path)
    out = []
    for mi, mesh in enumerate(doc.get("meshes", [])):
        for pi, prim in enumerate(mesh.get("primitives", [])):
            at = prim.get("attributes", {})
            if attribute not in at:
                continue
            pos = [tuple(float(c) for c in v) for v in G._accessor(doc, buffers, at["POSITION"])]
            pin = [float(v[0]) for v in G._accessor(doc, buffers, at[attribute])]
            skin = _skin(doc, buffers, at, len(pos))
            ids, first, pins = [], {}, []
            for v, (p, w, s) in enumerate(zip(pos, pin, skin)):
                if not 0 <= w <= 1:
                    raise ExportError(f"{Path(path).name}: mesh {mi} primitive {pi}: vertex {v} has pin {w}: weights are 0 to 1")
                key = (p, w, s)
                if key not in first:
                    first[key] = len(pins)
                    pins.append(w)
                ids.append(first[key])
            out.append({"mesh": mi, "primitive": pi, "particleIds": ids, "pin": pins, "massKg": mass})
    if not out:
        raise ExportError(f"{Path(path).name}: no primitive carries the attribute {attribute}: export with Attributes on (Blender: the exporter's Data > Mesh > Attributes)")
    return out


def write(out_path, key, cloth, chains):
    """Puts the model's entry into the physics.json at out_path (made when missing), checked by the cook's reader."""
    p = Path(out_path)
    doc = {"format": 1, "models": {}}
    if p.exists():
        try:
            doc = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, ValueError, RecursionError) as e:
            raise ExportError(f"{p}: cannot read it as JSON ({e}); fix or move it, nothing was written") from e
        if not isinstance(doc, dict) or not isinstance(doc.get("models", {}), dict):
            raise ExportError(f"{p}: not a physics.json (an object with \"models\" an object of models); fix or move it, nothing was written")
    doc.setdefault("models", {})[key] = {"cloth": cloth, "chains": chains}
    text = json.dumps(doc, indent=1, ensure_ascii=False) + "\n"
    P.loads(text, str(p))  # what the cook would refuse is refused here, by field
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")
    return doc


def _chains(specs):
    out = []
    for c in specs or []:
        root, sep, tip = c.partition(":")
        if not sep or not root or not tip:
            raise ExportError(f"--chain {c!r}: ROOT:TIP, two bone names")
        out.append({"rootJoint": root, "tipJoint": tip})
    return out


def export_from_blender(objects, group, gltf_path):
    """Inside Blender: the pin group of each object as the point attribute _KILN_PIN, the scene exported to gltf_path, the attribute taken off again."""
    import bpy  # noqa: PLC0415 (only in Blender)
    made = []
    try:
        for name in objects:
            ob = bpy.data.objects.get(name)
            if ob is None or ob.type != "MESH":
                raise ExportError(f"--object {name!r}: no mesh object of that name")
            vg = ob.vertex_groups.get(group)
            if vg is None:
                raise ExportError(f"--object {name!r} has no vertex group {group!r}")
            me = ob.data
            attr = me.attributes.new(ATTRIBUTE, "FLOAT", "POINT")
            made.append((me, attr.name))
            for v in me.vertices:
                attr.data[v.index].value = next((g.weight for g in v.groups if g.group == vg.index), 0.0)
        bpy.ops.export_scene.gltf(filepath=str(gltf_path), export_format="GLTF_SEPARATE" if str(gltf_path).lower().endswith(".gltf") else "GLB",
                                  export_attributes=True, export_apply=True)
    finally:
        for me, name in made:
            if name in me.attributes:
                me.attributes.remove(me.attributes[name])


def main(argv=None):
    if argv is None:
        argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    ap = argparse.ArgumentParser(prog="kiln_physics_export.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gltf", required=True, help="the model's glTF: written by Blender's exporter when --object is given, read otherwise")
    ap.add_argument("--key", required=True, help="the model's path in the mod, as a body or a recipe step names it (models/cape.gltf)")
    ap.add_argument("--object", action="append", help="in Blender: a cloth mesh object (repeat for more)")
    ap.add_argument("--pin-group", default="pin", help="in Blender: the vertex group of the pins (default: pin)")
    ap.add_argument("--mass", type=float, default=1.0, help="each cloth's whole mass in kg (default 1)")
    ap.add_argument("--chain", action="append", help="ROOT:TIP bone names of a chain (repeat for more)")
    ap.add_argument("--out", default="assets/physics.json", help="the physics.json to write (default: assets/physics.json)")
    a = ap.parse_args(argv)
    try:
        if a.object:
            export_from_blender(a.object, a.pin_group, Path(a.gltf))
        P.model_key(a.key, "--key")
        cloth = cloth_entries(a.gltf, mass=a.mass)
        doc = write(a.out, a.key, cloth, _chains(a.chain))
    except (ExportError, P.PhysicsError, G.GltfError) as e:
        print(f"kiln_physics_export: {e}", file=sys.stderr)
        return 2
    m = doc["models"][a.key]
    print(f"kiln_physics_export: {a.out}: {a.key}: {len(m['cloth'])} cloth ({', '.join(str(len(set(c['particleIds']))) + ' particles' for c in m['cloth'])}), "
          f"{len(m['chains'])} chain(s)")
    return 0


if __name__ == "__main__":
    rc = main()
    if "bpy" not in sys.modules:
        sys.exit(rc)
    elif rc:
        raise SystemExit(rc)
