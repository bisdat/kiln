#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes the glTF sources of the cloth and chain samples (no game data, stdlib only), and returns their physics entries:

    banner   a banner hung by its top edge (subdivisions 1: the research's 1 m two-triangle reference banner; more: a finer one)
    cape     a cape cut down its middle for its UVs (the seam copies weld back by particleIds), hung from a bar
    pendant  a three-link pendant on a skeleton of its own: root -> holder (the anchor) -> link_0 -> link_1 -> link_2, each link a small
             ring skinned to its joint; the chain swings link_0 to link_2

    python3 mods/samples/ModelPhysics/make_sources.py OUT_DIR [--banner-subdivisions N]

Used by this sample's build-assets.sh, mods/tests/model-physics-test and mods/tests/model-physics-client.
"""
import argparse
import base64
import json
import math
import struct
from pathlib import Path


def _gltf(path, prims, nodes, skin=None, materials=("cloth",), idle_node=None):
    blob, views, accessors = bytearray(), [], []

    def add(data, comp, typ, count, **extra):
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        blob.extend(data)
        while len(blob) % 4:
            blob.append(0)
        accessors.append({"bufferView": len(views) - 1, "componentType": comp, "count": count, "type": typ, **extra})
        return len(accessors) - 1

    out_prims = []
    for p in prims:
        pos = p["positions"]
        at = {"POSITION": add(b"".join(struct.pack("<3f", *v) for v in pos), 5126, "VEC3", len(pos),
                              min=[min(v[k] for v in pos) for k in range(3)], max=[max(v[k] for v in pos) for k in range(3)]),
              "TEXCOORD_0": add(b"".join(struct.pack("<2f", *v) for v in p["uvs"]), 5126, "VEC2", len(pos))}
        if p.get("joints"):
            at["JOINTS_0"] = add(b"".join(struct.pack("<4B", *v) for v in p["joints"]), 5121, "VEC4", len(pos))
            at["WEIGHTS_0"] = add(b"".join(struct.pack("<4f", *v) for v in p["weights"]), 5126, "VEC4", len(pos))
        idx = add(struct.pack(f"<{len(p['indices'])}H", *p["indices"]), 5123, "SCALAR", len(p["indices"]))
        out_prims.append({"attributes": at, "indices": idx, "material": p.get("material", 0)})
    doc = {"asset": {"version": "2.0", "generator": "kiln make_sources.py"}, "scene": 0,
           "scenes": [{"nodes": [i for i in range(len(nodes)) if not any(i in n.get("children", []) for n in nodes)]}],
           "nodes": nodes, "meshes": [{"name": Path(path).stem, "primitives": out_prims}],
           "materials": [{"name": m, "pbrMetallicRoughness": {"baseColorFactor": [0.6, 0.15, 0.1, 1] if m == "cloth" else [0.75, 0.7, 0.55, 1],
                                                              "metallicFactor": 0.0 if m == "cloth" else 0.9, "roughnessFactor": 0.8 if m == "cloth" else 0.35}}
                         for m in materials],
           "accessors": accessors, "bufferViews": views}
    if idle_node is not None:  # "Idle": the node turns 5 degrees about z and back in 2 s (a creature's graph needs an Idle clip)
        times = add(struct.pack("<3f", 0.0, 1.0, 2.0), 5126, "SCALAR", 3, min=[0.0], max=[2.0])
        s5 = math.sin(math.radians(2.5))
        rots = add(struct.pack("<12f", 0, 0, 0, 1, 0, 0, s5, math.cos(math.radians(2.5)), 0, 0, 0, 1), 5126, "VEC4", 3)
        doc["animations"] = [{"name": "Idle", "samplers": [{"input": times, "output": rots}],
                              "channels": [{"sampler": 0, "target": {"node": idle_node, "path": "rotation"}}]}]
    if skin:
        ibm = add(b"".join(struct.pack("<16f", *[m[r][c] for c in range(4) for r in range(4)]) for m in skin["ibm"]), 5126, "MAT4", len(skin["ibm"]))
        doc["skins"] = [{"joints": skin["joints"], "inverseBindMatrices": ibm}]
    doc["buffers"] = [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode()}]
    Path(path).write_text(json.dumps(doc, indent=1) + "\n")


def _grid(cols, rows, width, height, top=0.0, z=0.0):
    """A cols x rows grid hanging from y = top, centred on x; top row first. -> (positions, uvs, indices)."""
    w = cols + 1
    pos = [(-width / 2 + width * x / cols, top - height * y / rows, z) for y in range(rows + 1) for x in range(w)]
    uvs = [(x / cols, y / rows) for y in range(rows + 1) for x in range(w)]
    idx = []
    for y in range(rows):
        for x in range(cols):
            a, b, c, d = y * w + x, y * w + x + 1, (y + 1) * w + x, (y + 1) * w + x + 1
            idx += [a, c, b, b, c, d]
    return pos, uvs, idx


def banner(path, subdivisions=1):
    """-> its physics.json entry."""
    if subdivisions == 1:  # the research's reference banner, vertex for vertex
        pos, uvs, idx = [(0, 1, 0), (1, 1, 0), (0, 0, 0), (1, 0, 0)], [(0, 0), (1, 0), (0, 1), (1, 1)], [0, 2, 1, 1, 2, 3]
        pin = [1, 1, 0, 0]
    else:
        pos, uvs, idx = _grid(subdivisions, subdivisions, 1.0, 1.0, top=1.0)
        pin = [1.0 if i <= subdivisions else 0.0 for i in range(len(pos))]
    _gltf(path, [{"positions": pos, "uvs": uvs, "indices": idx}], [{"name": "banner", "mesh": 0}])
    return {"cloth": [{"mesh": 0, "primitive": 0, "particleIds": list(range(len(pos))), "pin": pin, "massKg": 0.3}]}


def cape(path, cols=6, rows=8, top=1.6, z=-0.15, collider_joint=None):
    """A cape whose middle column is in the file twice (a UV cut): particleIds weld it back. A bar across its top is a rigid second primitive.
    top, z: where it hangs (a worn cape attached to a joint has the joint at the origin); collider_joint: the joint its collider follows."""
    pos, uvs, idx = _grid(cols, rows, 0.8, 1.2, top=top, z=z)
    w = cols + 1
    mid = cols // 2
    seam = [y * w + mid for y in range(rows + 1)]
    copy_of = {v: len(pos) + i for i, v in enumerate(seam)}
    tris = [tuple(idx[t:t + 3]) for t in range(0, len(idx), 3)]
    idx = [copy_of[v] if v in copy_of and all(pos[u][0] >= pos[mid][0] - 1e-9 for u in t) else v for t in tris for v in t]
    pos2 = pos + [pos[v] for v in seam]
    uvs2 = uvs + [(uvs[v][0] + 0.02, uvs[v][1]) for v in seam]
    ids = list(range(len(pos))) + seam
    pin = [1.0 if i < w else 0.0 for i in range(len(pos))]
    bar = {"positions": [(-0.45, top + 0.02, z - 0.02), (0.45, top + 0.02, z - 0.02), (-0.45, top + 0.06, z + 0.02), (0.45, top + 0.06, z + 0.02)], "uvs": [(0, 0)] * 4,
           "indices": [0, 1, 2, 2, 1, 3], "material": 1}
    _gltf(path, [{"positions": pos2, "uvs": uvs2, "indices": idx}, bar], [{"name": "cape", "mesh": 0}], materials=("cloth", "metal"))
    return {"cloth": [{"mesh": 0, "primitive": 0, "particleIds": ids, "pin": pin, "massKg": 0.8,
                       "colliders": [{"type": "sphere", "x": 0, "y": top - 0.6, "z": z + 0.2, "radius": 0.2, "joint": collider_joint}]}]}


def _translate(x, y, z):
    return [[1.0, 0.0, 0.0, x], [0.0, 1.0, 0.0, y], [0.0, 0.0, 1.0, z], [0.0, 0.0, 0.0, 1.0]]


def pendant(path, links=3, idle=False):
    """A pendant on a skeleton of its own: root (y 0) -> holder (y 1.5) -> link_0 .. link_<links-1>, 0.12 m apart. With idle, an "Idle" animation
    turns the holder (never a link: a swinging joint carries no animation). -> its physics.json entry."""
    names = ["root", "holder"] + [f"link_{i}" for i in range(links)]
    world_y = [0.0, 1.5] + [1.5 - 0.1 - 0.12 * i for i in range(links)]
    nodes = [{"name": "root", "children": [1]}, {"name": "holder", "translation": [0, 1.5, 0], "children": [2]}]
    for i in range(links):
        node = {"name": names[2 + i], "translation": [0, -0.1 if i == 0 else -0.12, 0]}
        if i + 1 < links:
            node["children"] = [3 + i]
        nodes.append(node)
    pos, uvs, joints, weights, idx = [], [], [], [], []
    for j, y in enumerate(world_y):
        if j == 0:
            continue
        r = 0.06 if j == 1 else 0.035
        base = len(pos)
        for k in range(8):  # a flat ring of 8 quads facing +z
            a = 2 * math.pi * k / 8
            for rr in (r, r * 0.6):
                pos.append((math.cos(a) * rr, y + math.sin(a) * rr, 0.0))
                uvs.append((k / 8, 0.0 if rr == r else 1.0))
                joints.append((j, 0, 0, 0))
                weights.append((1.0, 0, 0, 0))
        for k in range(8):
            a, b = base + 2 * k, base + 2 * ((k + 1) % 8)
            idx += [a, a + 1, b, b, a + 1, b + 1]
    mesh_node = len(nodes)
    nodes.append({"name": "pendant", "mesh": 0, "skin": 0})
    _gltf(path, [{"positions": pos, "uvs": uvs, "indices": idx, "joints": joints, "weights": weights}], nodes,
          skin={"joints": list(range(mesh_node)), "ibm": [_translate(0, -y, 0) for y in world_y]}, materials=("pendant_metal",), idle_node=1 if idle else None)
    return {"chains": [{"rootJoint": "link_0", "tipJoint": f"link_{links - 1}", "particleMassKg": 0.2, "particleRadius": 0.02}]}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--banner-subdivisions", type=int, default=1)
    a = ap.parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    entries = {"banner.gltf": banner(out / "banner.gltf", a.banner_subdivisions), "cape.gltf": cape(out / "cape.gltf"), "pendant.gltf": pendant(out / "pendant.gltf")}
    print(json.dumps(entries))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
