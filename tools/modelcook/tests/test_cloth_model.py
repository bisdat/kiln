# SPDX-License-Identifier: MIT
"""Cloth in the model writer, without game files: models with cloth are cooked through steps.model and cook.json against the made-up game
of tests/fakegame.py, decoded with tools/kfc and compared: the banner (its fabric is the golden), a UV-seamed cape, two cloth meshes with a
rigid part and colliders, skinned cloth (the simulation skin holds skeleton joints, the render skin palette slots), the refusals of the
writer, the recipe key that turns the feature on, the worn path (G2) and the layout guard (G6). Models without physics cook to the bytes
they cooked to before cloth was added.

    python3 -m unittest discover -s tools/modelcook/tests -p test_cloth_model.py
"""
import base64
import copy
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent.parent
sys.path[:0] = [str(HERE), str(HERE.parent), str(HERE.parent / "samples"), str(HERE.parent / "blender"), str(TOOLS / "kfc"), str(TOOLS / "kfc" / "tests"), str(TOOLS / "animcook"),
                str(TOOLS / "animcook" / "samples")]

import cube_sword  # noqa: E402
import kiln_physics_export as KX  # noqa: E402
import fakegame  # noqa: E402
import rigged_figure  # noqa: E402
from anicook import skeleton as K  # noqa: E402
from anicook.gltfdoc import Doc  # noqa: E402
from cook import fabric as FB, gltf, physics as PH, recipe, rendermodel as RM, skin as S, steps  # noqa: E402
from cook.validate import validate  # noqa: E402
from kfc.blob import Decoder, DecodeError  # noqa: E402
from kfc.refl import TypeDb  # noqa: E402

FIXTURE_TYPES = TypeDb.load(HERE / "fixtures" / "types-rendermodel.json")
BANNER = HERE / "fixtures" / "banner"
NEW = "66666666-0000-4000-8000-000000000006"
SKELETON = "aaaaaaaa-0000-4000-8000-000000000001"
CLOTH_ON = frozenset({"modelcook.cloth"})


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def write_gltf(path, meshes, nodes, skins=(), materials=("cloth",)):
    """A glTF with its data in a data URI. meshes: [[primitive]], a primitive {"positions", "indices", "uvs"?, "joints"?, "weights"?, "morphs"?,
    "material"?}; nodes: glTF nodes as they are; skins: [{"joints": [node], "ibm": [4x4 rows]}]."""
    blob = bytearray()
    views, accessors = [], []

    def add(data, comp, typ, count, **extra):
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        blob.extend(data)
        while len(blob) % 4:
            blob.append(0)
        accessors.append({"bufferView": len(views) - 1, "componentType": comp, "count": count, "type": typ, **extra})
        return len(accessors) - 1

    out_meshes = []
    for prims in meshes:
        ps = []
        for pr in prims:
            pos = pr["positions"]
            at = {"POSITION": add(b"".join(struct.pack("<3f", *v) for v in pos), 5126, "VEC3", len(pos),
                                  min=[min(v[k] for v in pos) for k in range(3)], max=[max(v[k] for v in pos) for k in range(3)])}
            if pr.get("uvs"):
                at["TEXCOORD_0"] = add(b"".join(struct.pack("<2f", *v) for v in pr["uvs"]), 5126, "VEC2", len(pos))
            for name, vals in (pr.get("attributes") or {}).items():  # a scalar float attribute per vertex (_KILN_PIN)
                at[name] = add(struct.pack(f"<{len(vals)}f", *vals), 5126, "SCALAR", len(pos))
            if pr.get("joints"):
                at["JOINTS_0"] = add(b"".join(struct.pack("<4B", *v) for v in pr["joints"]), 5121, "VEC4", len(pos))
                at["WEIGHTS_0"] = add(b"".join(struct.pack("<4f", *v) for v in pr["weights"]), 5126, "VEC4", len(pos))
            big = len(pos) > 65535
            idx = add(struct.pack(f"<{len(pr['indices'])}{'I' if big else 'H'}", *pr["indices"]), 5125 if big else 5123, "SCALAR", len(pr["indices"]))
            prim = {"attributes": at, "indices": idx, "material": pr.get("material", 0)}
            if pr.get("morphs"):
                prim["targets"] = [{"POSITION": add(b"".join(struct.pack("<3f", *d) for d in ds), 5126, "VEC3", len(pos))} for ds in pr["morphs"].values()]
                prim["extras"] = {"targetNames": list(pr["morphs"])}
            ps.append(prim)
        out_meshes.append({"primitives": ps})
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [i for i, n in enumerate(nodes) if not any(i in m.get("children", []) for m in nodes)]}],
           "nodes": nodes, "meshes": out_meshes, "materials": [{"name": m} for m in materials], "accessors": accessors, "bufferViews": views}
    if skins:
        doc["skins"] = []
        for sk in skins:
            ibm = add(b"".join(struct.pack("<16f", *[m[r][c] for c in range(4) for r in range(4)]) for m in sk["ibm"]), 5126, "MAT4", len(sk["ibm"]))
            doc["skins"].append({"joints": sk["joints"], "inverseBindMatrices": ibm})
    doc["buffers"] = [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode()}]
    Path(path).write_text(json.dumps(doc))
    return Path(path)


def grid(cols, rows, x0=0.0, y0=0.0, z=0.0, size=1.0):
    """A cols x rows grid of quads facing +z, top row at y0 + rows*size. -> (positions, indices, uvs), top row first."""
    w = cols + 1
    pos = [(x0 + x * size, y0 + (rows - y) * size, z) for y in range(rows + 1) for x in range(w)]
    idx = []
    for y in range(rows):
        for x in range(cols):
            a, b, c, d = y * w + x, y * w + x + 1, (y + 1) * w + x, (y + 1) * w + x + 1
            idx += [a, c, b, b, c, d]
    uvs = [(x / cols, y / rows) for y in range(rows + 1) for x in range(w)]
    return pos, idx, uvs


def top_pinned(cols, rows):
    return [1.0 if i <= cols else 0.0 for i in range((cols + 1) * (rows + 1))]


def physics(model, cloth=(), chains=()):
    """A ModelPhysics as assets/physics.json would give it."""
    return PH.parse({"format": 1, "models": {model: {"cloth": list(cloth), "chains": list(chains)}}}).get(model)


def translate(x, y, z):
    return [[1.0, 0.0, 0.0, x], [0.0, 1.0, 0.0, y], [0.0, 0.0, 1.0, z], [0.0, 0.0, 0.0, 1.0]]


class Game:
    """The made-up client of tests/fakegame.py, built once per class."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.types = fakegame.types()
        cls.k = fakegame.build(cls.dir / "game", "client", cls.types)

    @classmethod
    def tearDownClass(cls):
        cls.k.close()
        cls.tmp.cleanup()

    def cook(self, path, phys, features=CLOTH_ON, types=None, **kw):
        files, entries, report = steps.model(self.k, types or self.types, [str(p) for p in ([path] if not isinstance(path, list) else path)], None, guid=NEW,
                                             mode="new", material=fakegame.MATERIAL, physics=phys, features=features, **kw)
        data, blob = files[report["bin"]], files[report["blob"]]
        return Decoder(self.types).decode("keen::RenderModel", data), data, blob, report


class Banner(Game, unittest.TestCase):
    def test_banner(self):
        phys = PH.load(BANNER / "physics.json").get("banner.gltf")
        d, data, blob, r = self.cook(BANNER / "banner.gltf", phys)
        self.assertTrue(r["ok"], json.dumps({k: v for k, v in r["checks"].items() if not v}))
        for c in ("cloth_selectors_one_based", "cloth_arrays_agree", "cloth_totals", "cloth_colliders_within_32", "no_sleeves"):
            self.assertIs(r["checks"][c], True, c)
        self.assertEqual([m["clothMeshIndex"] for m in d["meshes"]], [1])
        cl = d["cloth"]
        self.assertEqual((cl["hasSkinning"], cl["totalSimulationTriangleCount"], cl["totalRenderVertexCount"]), (False, 2, 4))
        q = cl["meshes"][0]["data"]
        want = json.loads((BANNER / "expected-fabric.json").read_text())
        got = q["fabric"]
        for k, v in want["fabric"].items():
            self.assertEqual(got[k], [f32(x) for x in v] if k in ("restValues", "tetherLengths") else v, k)
        self.assertEqual(q["simulationInvMasses"], [f32(x) for x in want["inverseMasses"]])
        self.assertEqual(q["simulationVertexWeights"], [float(x) for x in want["vertexWeights"]])
        self.assertEqual(q["skinnedVertexIndices"], [0, 1])
        self.assertEqual(q["simulationSkinningData"], [])
        self.assertEqual(q["renderSimulationVertexIndices"], [0, 1, 2, 3])
        # one mirror: glTF x = 1 is the game's x = -1, in the simulation and in the drawn vertices alike
        self.assertEqual([p["x"] for p in q["simulationPositions"]], [-0.0, -1.0, -0.0, -1.0])
        self.assertEqual(q["renderPositions"], q["simulationPositions"])
        self.assertEqual(d["collider"], {"hasSkinning": False, "ignoreClothCollider": False, "spheres": [], "capsules": [], "instances": []})
        self.assertEqual(d["sleeves"], [])
        self.assertIsNone(d["chain"])
        self.assertEqual(r["cloth"], {"meshes": [{"particles": 4, "renderVertices": 4, "phases": 4, "pinned": 2}], "spheres": 0, "capsules": 0, "skinned": False})
        self.assertEqual(cl["meshes"][0]["gravity"], f32(9.81))
        self.assertEqual(cl["meshes"][0]["solverFrequency"], 300.0)
        # the same input twice: the same bytes
        _, data2, blob2, _ = self.cook(BANNER / "banner.gltf", phys)
        self.assertEqual((data, blob), (data2, blob2))
        self.assertEqual(r["content_hash"], dict(zip(("size", "hash0", "hash1", "hash2"), RM.content_hash(blob))))

    def test_stiffness_gravity_damping_as_asked(self):
        phys = physics("banner.gltf", [{"mesh": 0, "primitive": 0, "pin": [1, 1, 0, 0], "massKg": 0.3, "stiffness": 0.5, "gravity": 4, "damping": 0.1}])
        m = self.cook(BANNER / "banner.gltf", phys)[0]["cloth"]["meshes"][0]
        self.assertEqual((m["phasesStiffness"], m["gravity"], m["damping"]), (0.5, 4.0, f32(0.1)))
        self.assertEqual(m["data"]["simulationStiffnesses"], [])
        self.assertEqual(m["data"]["fabric"]["stiffnessValues"], [])

    def test_truncated_or_malformed_bytes_do_not_decode(self):
        phys = PH.load(BANNER / "physics.json").get("banner.gltf")
        _, data, _, _ = self.cook(BANNER / "banner.gltf", phys)
        with self.assertRaises((DecodeError, struct.error)):
            Decoder(self.types).decode("keen::RenderModel", data[:len(data) - 40])
        bad = bytearray(data)
        off = Decoder(self.types)  # the cloth optional's offset, pointed past the end
        struct.pack_into("<i", bad, 0xdc, len(bad))
        with self.assertRaises((DecodeError, struct.error)):
            off.decode("keen::RenderModel", bytes(bad))


class Shapes(Game, unittest.TestCase):
    def test_uv_seamed_cape(self):
        """A 4 x 3 cape whose middle column is cut for its UVs: the glTF has the column twice, particleIds weld it."""
        pos, idx, uvs = grid(4, 3)
        w = 5
        seam = [y * w + 2 for y in range(4)]
        copy_of = {v: len(pos) + i for i, v in enumerate(seam)}
        right = lambda t: all(pos[v][0] >= 2 for v in t)  # noqa: E731
        tris = [tuple(idx[t:t + 3]) for t in range(0, len(idx), 3)]
        idx2 = [copy_of[v] if v in copy_of and right(t) else v for t in tris for v in t]
        pos2 = pos + [pos[v] for v in seam]
        uvs2 = uvs + [(0.52, uvs[v][1]) for v in seam]
        path = write_gltf(self.dir / "cape.gltf", [[{"positions": pos2, "indices": idx2, "uvs": uvs2}]], [{"mesh": 0}])
        ids = list(range(len(pos))) + seam
        phys = physics("cape.gltf", [{"mesh": 0, "primitive": 0, "particleIds": ids, "pin": top_pinned(4, 3), "massKg": 0.8}])
        d, _, _, r = self.cook(path, phys)
        self.assertTrue(r["ok"], json.dumps({k: v for k, v in r["checks"].items() if not v}))
        q = d["cloth"]["meshes"][0]["data"]
        self.assertEqual(q["fabric"]["numParticles"], 20)
        self.assertEqual(len(q["renderPositions"]), 24)
        self.assertEqual(d["meshes"][0]["vertexCount"], 24)
        self.assertEqual(q["renderSimulationVertexIndices"][20:], seam)
        self.assertEqual(q["renderUvs"][20], {"x": f32(0.52), "y": 0.0})
        # the seam copies are drawn where their particle is
        for v, pid in enumerate(q["renderSimulationVertexIndices"]):
            self.assertEqual(q["renderPositions"][v], q["simulationPositions"][pid])
        # the simulation's triangles are the drawn ones, through the map (as every cloth mesh of the game has them)
        rs = q["renderSimulationVertexIndices"]
        self.assertEqual([(rs[t["x"]], rs[t["y"]], rs[t["z"]]) for t in q["renderTriangles"]], [(t["x"], t["y"], t["z"]) for t in q["simulationTriangles"]])

    def two_cloth_and_a_rigid_part(self, colliders_a=(), colliders_b=()):
        a = grid(2, 2, x0=-3)
        b = grid(3, 1, x0=1)
        rigid = ([(0, 0, 0), (0.2, 0, 0), (0, 0.2, 0), (0.2, 0.2, 0)], [0, 1, 2, 2, 1, 3], None)
        path = write_gltf(self.dir / "pair.gltf", [[{"positions": a[0], "indices": a[1], "uvs": a[2]}, {"positions": rigid[0], "indices": rigid[1], "material": 1}],
                                                   [{"positions": b[0], "indices": b[1], "uvs": b[2]}]],
                          [{"mesh": 0}, {"mesh": 1}], materials=("cloth", "metal"))
        # declared B first: the cloth meshes are numbered in the order they are declared
        phys = physics("pair.gltf", [{"mesh": 1, "primitive": 0, "pin": top_pinned(3, 1), "colliders": list(colliders_b)},
                                     {"mesh": 0, "primitive": 0, "pin": top_pinned(2, 2), "colliders": list(colliders_a)}])
        return path, phys

    def test_two_cloth_meshes_and_a_rigid_part(self):
        sphere = {"type": "sphere", "x": 0.5, "y": 0.5, "z": 0.3, "radius": 0.2}
        capsule = {"type": "capsule", "a": {"x": -2, "y": 0, "z": 0.3, "radius": 0.1}, "b": {"x": -2, "y": 1, "z": 0.3, "radius": 0.15}}
        path, phys = self.two_cloth_and_a_rigid_part(colliders_a=[capsule, sphere], colliders_b=[sphere])
        d, _, blob, r = self.cook(path, phys)
        self.assertTrue(r["ok"], json.dumps({k: v for k, v in r["checks"].items() if not v}))
        self.assertEqual([m["clothMeshIndex"] for m in d["meshes"]], [2, 0, 1])
        self.assertEqual([m["data"]["fabric"]["numParticles"] for m in d["cloth"]["meshes"]], [8, 9])
        self.assertEqual(d["cloth"]["totalRenderVertexCount"], 17)
        self.assertEqual(d["cloth"]["totalSimulationTriangleCount"], 6 + 8)
        # every mesh has its own vertices (the cloth is not shared with anything)
        self.assertEqual(len({m["vertexDataByteOffset"] for m in d["meshes"]}), 3)
        co = d["collider"]
        self.assertEqual((len(co["spheres"]), len(co["capsules"])), (1, 1))
        self.assertEqual(co["spheres"][0]["center"], {"x": -0.5, "y": 0.5, "z": f32(0.3)})  # mirrored once
        self.assertEqual(co["capsules"][0]["center0"]["x"], 2.0)
        self.assertEqual((co["capsules"][0]["radius0"], co["capsules"][0]["radius1"]), (f32(0.1), f32(0.15)))
        # one instance per cloth mesh, in cloth order: B has the sphere, A the capsule and the sphere
        self.assertEqual(co["instances"], [{"sphereIndices": [0], "capsuleIndices": []}, {"sphereIndices": [0], "capsuleIndices": [0]}])
        self.assertFalse(co["hasSkinning"])
        self.assertEqual(co["spheres"][0]["skinningData"], {"jointIndices": [0, 0, 0, 0], "jointWeights": [0, 0, 0, 0]})

    def test_colliders_up_to_32(self):
        many = [{"type": "sphere", "x": i * 0.1, "y": 0, "z": 0.5, "radius": 0.05} for i in range(30)]
        many.append({"type": "capsule", "a": {"x": 0, "y": 0, "z": 1, "radius": 0.1}, "b": {"x": 0, "y": 1, "z": 1, "radius": 0.1}})
        path, phys = self.two_cloth_and_a_rigid_part(colliders_a=many)
        d, _, _, r = self.cook(path, phys)
        self.assertTrue(r["checks"]["cloth_colliders_within_32"])
        self.assertEqual(len(d["collider"]["spheres"]) + 2 * len(d["collider"]["capsules"]), 32)
        self.assertEqual(d["collider"]["instances"][0], {"sphereIndices": [], "capsuleIndices": []})

    def test_vertex_overflow(self):
        n = 65536
        pos = [(i % 256 * 0.01, i // 256 * 0.01, 0.0) for i in range(n)]
        path = write_gltf(self.dir / "big.gltf", [[{"positions": pos, "indices": [0, 1, 256]}]], [{"mesh": 0}])
        phys = physics("big.gltf", [{"mesh": 0, "primitive": 0, "pin": [1, 0, 0]}])
        with self.assertRaisesRegex((steps.StepError, RM.CookError), "65536"):
            self.cook(path, phys)


def skinned_cape(path, morphs=None, seam=False, copies=1):
    """A 2 x 2 cape on a three-joint chain (root, upper at y 1.5, lower at y 1.0): its top row on `upper`, the middle shared half and half,
    the bottom on `lower`. With seam, the middle column again for a UV cut, welded back. copies: primitives of the same data in its mesh."""
    pos, idx, uvs = grid(2, 2, x0=-1, y0=0.5, z=0.1, size=0.5)
    joints = [(1, 2, 0, 0)] * 9
    weights = [(1.0, 0, 0, 0)] * 3 + [(0.5, 0.5, 0, 0)] * 3 + [(0, 1.0, 0, 0)] * 3
    ids = None
    if seam:
        col = [1, 4, 7]
        extra = {v: 9 + i for i, v in enumerate(col)}
        tris = [tuple(idx[t:t + 3]) for t in range(0, len(idx), 3)]
        idx = [extra[v] if v in extra and all(pos[u][0] >= -0.5 for u in t) else v for t in tris for v in t]
        pos, uvs = pos + [pos[v] for v in col], uvs + [(0.6, uvs[v][1]) for v in col]
        joints, weights = joints + [joints[v] for v in col], weights + [weights[v] for v in col]
        ids = list(range(9)) + col
    nodes = [{"name": "root", "children": [1]}, {"name": "upper", "translation": [0, 1.5, 0], "children": [2]}, {"name": "lower", "translation": [0, -0.5, 0]},
             {"name": "cape", "mesh": 0, "skin": 0}]
    ibm = [translate(0, 0, 0), translate(0, -1.5, 0), translate(0, -1.0, 0)]
    write_gltf(path, [[{"positions": pos, "indices": idx, "uvs": uvs, "joints": joints, "weights": weights, "morphs": morphs}] * copies], nodes,
               skins=[{"joints": [0, 1, 2], "ibm": ibm}])
    return ids


class Skinned(Game, unittest.TestCase):
    def cook_skinned(self, path, phys, **kw):
        target = S.Target.from_skeleton(K.from_gltf(Doc(path)), SKELETON)
        return self.cook(path, phys, target=target, **kw) + (target,)

    def test_skinned_cloth(self):
        path = self.dir / "skinned.gltf"
        ids = skinned_cape(path, seam=True)
        sphere = {"type": "sphere", "x": 0, "y": 1.2, "z": -0.1, "radius": 0.15, "joint": "upper"}
        capsule = {"type": "capsule", "a": {"x": 0, "y": 1.5, "z": -0.1, "radius": 0.1, "joint": "j2"}, "b": {"x": 0, "y": 0.6, "z": -0.1, "radius": 0.1, "joint": "j2"}}
        phys = physics("skinned.gltf", [{"mesh": 0, "primitive": 0, "particleIds": ids, "pin": [1, 1, 1, 0, 0, 0, 0, 0, 0], "colliders": [sphere, capsule]}])
        d, _, blob, r, target = self.cook_skinned(path, phys)
        self.assertTrue(r["ok"], json.dumps({k: v for k, v in r["checks"].items() if not v}))
        self.assertEqual(target.names, ["root", "upper", "lower"])
        # palette: the weighted joints, sorted (upper and lower are hierarchy 1 and 2, palette 0 and 1)
        self.assertEqual(d["skinningMap"], [1, 2])
        q = d["cloth"]["meshes"][0]["data"]
        self.assertTrue(d["cloth"]["hasSkinning"])
        skin = [sorted(zip(s["jointIndices"], s["jointWeights"])) for s in q["simulationSkinningData"]]
        # the simulation skin holds HIERARCHY joints (1 = upper, 2 = lower), not palette slots (0, 1)
        self.assertEqual(skin[0], [(0, 0), (0, 0), (0, 0), (1, 255)])
        self.assertEqual(skin[3], [(0, 0), (0, 0), (1, 128), (2, 127)] if skin[3][-1][1] == 127 else [(0, 0), (0, 0), (1, 127), (2, 128)])
        self.assertEqual(skin[8], [(0, 0), (0, 0), (0, 0), (2, 255)])
        # the render skin of the same vertex holds palette slots: the same joints through skinningMap
        st = d["skinningData"]["start"] + 8 * d["meshes"][0]["skinVertexOffset"]
        for v, pid in enumerate(q["renderSimulationVertexIndices"]):
            raw = blob[st + 8 * v:st + 8 * v + 8]
            render = sorted((d["skinningMap"][j], w) for j, w in zip(raw[:4], raw[4:]) if w)
            self.assertEqual(render, [(j, w) for j, w in skin[pid] if w], f"vertex {v}")
        self.assertEqual(q["fabric"]["numParticles"], 9)
        self.assertEqual(len(q["renderPositions"]), 12)
        co = d["collider"]
        self.assertTrue(co["hasSkinning"])
        self.assertEqual(co["spheres"][0]["skinningData"], {"jointIndices": [1, 0, 0, 0], "jointWeights": [255, 0, 0, 0]})
        self.assertEqual(co["capsules"][0]["skinningData"], {"jointIndices": [2, 0, 0, 0], "jointWeights": [255, 0, 0, 0]})
        self.assertEqual(d["globalScale"], 1.0)

    def test_cloth_vertices_are_its_own(self):
        """A skinned model shares a vertex group between primitives of equal data; a cloth primitive's vertices are never shared."""
        path = self.dir / "copies.gltf"
        skinned_cape(path, copies=2)
        d, _, _, r, _ = self.cook_skinned(path, physics("copies.gltf", [{"mesh": 0, "primitive": 0, "pin": [1, 1, 1, 0, 0, 0, 0, 0, 0]}]))
        self.assertTrue(r["ok"])
        self.assertEqual([m["clothMeshIndex"] for m in d["meshes"]], [1, 0])
        self.assertNotEqual(d["meshes"][0]["vertexDataByteOffset"], d["meshes"][1]["vertexDataByteOffset"])
        self.assertNotEqual(d["meshes"][0]["skinVertexOffset"], d["meshes"][1]["skinVertexOffset"])
        # without cloth the two share one group, as before
        d0 = self.cook_skinned(path, None)[0]
        self.assertEqual(d0["meshes"][0]["vertexDataByteOffset"], d0["meshes"][1]["vertexDataByteOffset"])

    def test_welded_copies_need_one_skin(self):
        path = self.dir / "badweld.gltf"
        ids = skinned_cape(path, seam=True)
        doc = json.loads(path.read_text())
        # the seam copy of vertex 4 (index 10) gets another skin: rewrite its weights in the buffer
        raw = bytearray(base64.b64decode(doc["buffers"][0]["uri"].split(",", 1)[1]))
        wacc = doc["accessors"][doc["meshes"][0]["primitives"][0]["attributes"]["WEIGHTS_0"]]
        at = doc["bufferViews"][wacc["bufferView"]]["byteOffset"] + 10 * 16
        struct.pack_into("<4f", raw, at, 0.9, 0.1, 0, 0)
        doc["buffers"][0]["uri"] = "data:application/octet-stream;base64," + base64.b64encode(bytes(raw)).decode()
        path.write_text(json.dumps(doc))
        phys = physics("badweld.gltf", [{"mesh": 0, "primitive": 0, "particleIds": ids, "pin": [1, 1, 1, 0, 0, 0, 0, 0, 0]}])
        with self.assertRaisesRegex(steps.StepError, r"cloth\[0\]\.particleIds\[10\]: vertex 10 welds to vertex 4 .* another skin"):
            self.cook_skinned(path, phys)

    def test_collider_joint_rules(self):
        path = self.dir / "cj.gltf"
        skinned_cape(path)
        base = {"mesh": 0, "primitive": 0, "pin": [1, 1, 1, 0, 0, 0, 0, 0, 0]}
        rigid = {"type": "sphere", "x": 0, "y": 1, "z": 0, "radius": 0.1}
        with self.assertRaisesRegex(steps.StepError, r"cloth\[0\]\.colliders\[0\]\.joint: the model is skinned"):
            self.cook_skinned(path, physics("cj.gltf", [dict(base, colliders=[rigid])]))
        with self.assertRaisesRegex(steps.StepError, r"colliders\[0\]\.joint: 'elbow' is not a joint"):
            self.cook_skinned(path, physics("cj.gltf", [dict(base, colliders=[dict(rigid, joint="elbow")])]))
        with self.assertRaisesRegex(steps.StepError, r"banner.gltf\"\]\.cloth\[0\]\.colliders\[0\]\.joint: 'upper': the model has no skeleton"):
            self.cook(BANNER / "banner.gltf", physics("banner.gltf", [{"mesh": 0, "primitive": 0, "pin": [1, 1, 0, 0], "colliders": [dict(rigid, joint="upper")]}]))

    def test_attached_cloth_follows_its_joint(self):
        """A rigid model on one joint (a step's "attach"): its cloth's pinned particles follow that joint, by its skeleton index."""
        path = self.dir / "skinned2.gltf"
        skinned_cape(path)
        target = S.Target.from_skeleton(K.from_gltf(Doc(path)), SKELETON)
        pos, idx, uvs = grid(2, 2, size=0.3)
        flat = write_gltf(self.dir / "flag.gltf", [[{"positions": pos, "indices": idx, "uvs": uvs}]], [{"mesh": 0}])
        d, _, _, r = self.cook(flat, physics("flag.gltf", [{"mesh": 0, "primitive": 0, "pin": top_pinned(2, 2)}]), target=target, attach="j2")
        self.assertTrue(r["ok"])
        q = d["cloth"]["meshes"][0]["data"]
        self.assertEqual({tuple(s["jointIndices"]) for s in q["simulationSkinningData"]}, {(2, 0, 0, 0)})
        self.assertEqual(d["skinningMap"], [2])
        # moved to the joint's bind position, as the rendered vertices are
        self.assertEqual(q["simulationPositions"][0], {"x": -0.0, "y": f32(0.6 + 1.0), "z": 0.0})

    def test_global_scale(self):
        path = self.dir / "gs.gltf"
        skinned_cape(path)
        phys = physics("gs.gltf", [{"mesh": 0, "primitive": 0, "pin": [1, 1, 1, 0, 0, 0, 0, 0, 0]}])
        for gs in (0.5, 2.0):
            with self.subTest(globalScale=gs), self.assertRaisesRegex(steps.StepError, f"globalScale is {gs:g}: cloth needs 1"):
                self.cook_skinned(path, phys, global_scale=gs)
        self.assertEqual(self.cook_skinned(path, phys, global_scale=1.0)[0]["globalScale"], 1.0)

    def test_morph_target_on_cloth(self):
        path = self.dir / "morph.gltf"
        skinned_cape(path, morphs={"Female": [(0.0, 0.01, 0.0)] * 9})
        phys = physics("morph.gltf", [{"mesh": 0, "primitive": 0, "pin": [1, 1, 1, 0, 0, 0, 0, 0, 0]}])
        with self.assertRaisesRegex(steps.StepError, r"cloth\[0\]: the primitive has morph targets \(Female\)"):
            self.cook_skinned(path, phys)


class Refusals(Game, unittest.TestCase):
    def banner(self):
        return PH.load(BANNER / "physics.json").get("banner.gltf")

    def test_feature_off(self):
        with self.assertRaisesRegex(steps.StepError, r'^assets/physics.json: models\["banner.gltf"\]\.cloth: needs cook feature modelcook.cloth, which is off \(game test pending: 16e-cc16e\)'):
            self.cook(BANNER / "banner.gltf", self.banner(), features=frozenset())

    def test_a_second_lod(self):
        with self.assertRaisesRegex(steps.StepError, "one LOD"):
            self.cook([BANNER / "banner.gltf", BANNER / "banner.gltf"], self.banner())

    def test_no_such_primitive_and_an_instanced_mesh(self):
        with self.assertRaisesRegex(steps.StepError, r"cloth\[0\]: banner.gltf has no mesh 0 with a primitive 1"):
            self.cook(BANNER / "banner.gltf", physics("banner.gltf", [{"mesh": 0, "primitive": 1, "pin": [1, 1, 0, 0]}]))
        pos, idx, uvs = grid(1, 1)
        path = write_gltf(self.dir / "twice.gltf", [[{"positions": pos, "indices": idx}]], [{"mesh": 0}, {"mesh": 0, "translation": [3, 0, 0]}])
        with self.assertRaisesRegex(steps.StepError, "used by 2 nodes"):
            self.cook(path, physics("twice.gltf", [{"mesh": 0, "primitive": 0, "pin": [1, 1, 0, 0]}]))

    def test_mesh_rules_reach_the_author(self):
        with self.assertRaisesRegex(steps.StepError, r'^assets/physics.json: models\["banner.gltf"\]\.cloth\[0\]\.pin: the piece of cloth'):
            self.cook(BANNER / "banner.gltf", physics("banner.gltf", [{"mesh": 0, "primitive": 0, "pin": [0.5, 0, 0, 0]}]))

    def test_chains_wait_for_their_writer(self):
        with self.assertRaisesRegex(steps.StepError, r"\.chains: this cook does not write bone chains yet"):
            self.cook(BANNER / "banner.gltf", physics("banner.gltf", chains=[{"rootJoint": "a", "tipJoint": "b"}]), features=frozenset({"modelcook.cloth", "modelcook.chains2"}))

    def test_layout_guard(self):
        """G6: a game whose cloth types moved gets no cloth; a model without physics cooks as before on it."""
        self.assertEqual(RM.layout_problems(self.types, "cloth"), [])
        self.assertEqual(RM.layout_problems(self.types, "chains2"), [])
        dump = copy.deepcopy(self.types.dump)
        cm = next(t for t in dump["types"] if t["name"] == "keen::RenderClothMesh")
        next(f for f in cm["fields"] if f["name"] == "gravity")["offset"] = 0xa0
        moved = TypeDb(dump)
        self.assertEqual(RM.layout_problems(moved, "cloth"), ["keen::RenderClothMesh.gravity is keen::float32 at +0xa0, the cook writes keen::float32 at +0xa4"])
        with self.assertRaisesRegex(steps.StepError, r"\.cloth: this game's cloth types differ .*gravity.*it writes no cloth for this build"):
            self.cook(BANNER / "banner.gltf", self.banner(), types=moved)
        sword = self.dir / "sword.gltf"
        cube_sword.write(sword)
        _, data, _, r = self.cook(sword, None, types=moved)
        _, data0, _, _ = self.cook(sword, None)
        self.assertEqual(data, data0)
        dump2 = copy.deepcopy(self.types.dump)
        next(t for t in dump2["types"] if t["name"] == "keen::RenderClothFabricResource")["size"] = 72
        self.assertIn("keen::RenderClothFabricResource has 72 bytes, the cook writes 68", RM.layout_problems(TypeDb(dump2), "cloth"))


class BlenderHelper(Game, unittest.TestCase):
    """tools/modelcook/blender/kiln_physics_export.py without Blender: a glTF whose primitive carries _KILN_PIN (what its Blender path exports)."""

    def seamed(self):
        pos, idx, uvs = grid(2, 2)
        tris = [tuple(idx[t:t + 3]) for t in range(0, len(idx), 3)]
        col = [1, 4, 7]
        extra = {v: 9 + i for i, v in enumerate(col)}
        idx2 = [extra[v] if v in extra and all(pos[u][0] >= 1 for u in t) else v for t in tris for v in t]
        pos2, uvs2 = pos + [pos[v] for v in col], uvs + [(0.7, uvs[v][1]) for v in col]
        pin = top_pinned(2, 2) + [1.0, 0.0, 0.0]
        rigid = {"positions": [(5, 0, 0), (6, 0, 0), (5, 1, 0)], "indices": [0, 1, 2]}
        return write_gltf(self.dir / "seamed.gltf", [[rigid], [{"positions": pos2, "indices": idx2, "uvs": uvs2, "attributes": {KX.ATTRIBUTE: pin}}]],
                          [{"mesh": 0}, {"mesh": 1}])

    def test_it_writes_what_the_cook_takes(self):
        path = self.seamed()
        out = self.dir / "physics.json"
        out.write_text(json.dumps({"format": 1, "models": {"models/other.gltf": {"chains": [{"rootJoint": "a", "tipJoint": "b"}]}}}))
        self.assertEqual(KX.main(["--gltf", str(path), "--key", "seamed.gltf", "--mass", "0.7", "--chain", "upper:lower", "--out", str(out)]), 0)
        ph = PH.load(out)
        self.assertEqual(sorted(ph.models), ["models/other.gltf", "seamed.gltf"])
        mp = ph.get("seamed.gltf")
        c = mp.cloth[0]
        self.assertEqual((c.mesh, c.primitive, c.mass_kg), (1, 0, 0.7))
        self.assertEqual(c.particle_ids, tuple(range(9)) + (1, 4, 7))  # the seam copies weld back
        self.assertEqual(c.pin, (1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0))
        self.assertEqual((mp.chains[0].root_joint, mp.chains[0].tip_joint), ("upper", "lower"))
        d, _, _, r = self.cook(path, PH.Physics({"seamed.gltf": PH.ModelPhysics("seamed.gltf", mp.cloth, (), "x", 'x["seamed.gltf"]')}).get("seamed.gltf"))
        self.assertTrue(r["ok"])
        self.assertEqual([m["clothMeshIndex"] for m in d["meshes"]], [0, 1])
        self.assertEqual(d["cloth"]["meshes"][0]["data"]["fabric"]["numParticles"], 9)

    def test_it_refuses_what_it_cannot_read(self):
        sword = self.dir / "plain.gltf"
        cube_sword.write(sword)
        with self.assertRaisesRegex(KX.ExportError, "no primitive carries the attribute _KILN_PIN"):
            KX.cloth_entries(sword)
        out = self.dir / "bad.json"
        self.assertEqual(KX.main(["--gltf", str(self.seamed()), "--key", "../x.gltf", "--out", str(out)]), 2)
        self.assertFalse(out.exists())
        self.assertEqual(KX.main(["--gltf", str(self.seamed()), "--key", "x.gltf", "--chain", "lonely", "--out", str(out)]), 2)


# What the models without physics cooked to on the tree before cloth was added (base bd9d1073), recorded with the code below.
NONPHYSICS = {
    "rigid-cubes": ["e08068f0c442a6f976b8f867ec1a7e109f5b23824bf4f6d8eb84d36db5564458", "a10a3aa7d298745b26e379b2e30feada58be5d81dc1483b553c964d51af75128"],
    "rigid-tower": ["d2c4a1d6865d582dc08a80fbaa6d6385cfcffc95402edcd4965b9cab1e2604ba", "5661b0b0cafc7872b4ce8f1ba6b0fd77844627ae05cdcc15e8b4d304c4a94dce"],
    "skinned-figure": ["6c7da6f9cada05c36fa7f17106cc740128d274e4f17cd6c72e988b0dd13ca7aa", "7fab0067d632eafca6d43864bb78535630281e02669fee51040c9f7d3f7c3643"],
    "worn-morphs": ["f861caa25ddda4a7728e0999a18e2f1548eea76a5e04f545c79391e4c3c633ed", "11f34c00b355732b422737e215b67bc321ff860a7ed0ff447db821dbe7e74064"],
    "recipe/a.bin": "d9662336178377a2dfeee359c64837eaad673ea57890fdf206fe8d0b55eb53b0",
    "recipe/a.blob": "5661b0b0cafc7872b4ce8f1ba6b0fd77844627ae05cdcc15e8b4d304c4a94dce",
    "recipe/b.bin": "d9662336178377a2dfeee359c64837eaad673ea57890fdf206fe8d0b55eb53b0",
    "recipe/b.blob": "5661b0b0cafc7872b4ce8f1ba6b0fd77844627ae05cdcc15e8b4d304c4a94dce",
    "recipe/golem.bin": "e5ed3772bf76c83e17bb129f2300713fe43de06aedfe9983775a24b1ab9f63ef",
    "recipe/golem.blob": "326ec307387482441974247f5cecebd74e11575718ab34cf51832c2918cf80a5",
    "recipe/golem.model.json": "8654c1e084c9ba45d492a751dc72353e3cc46ed27136117aa5b37afe76a06e00",
    "recipe/v.bin": "f362f069fdf92ac3b51f550441cfa8e3930a7bf1b50b9e8fdc78be54f6bbf055",
}


def nonphysics_hashes(d):
    """sha256 of rigid, skinned, morphed and recipe-cooked models (no physics), made in folder d."""
    import test_worn as TW
    h = lambda b: hashlib.sha256(b).hexdigest()  # noqa: E731
    mat = {"material": "71165a08-1a37-495c-9cb9-39cd1145ebf0", "renderEffectId": "Pbr", "materialFlags": []}
    base = {"debugName": "", "materials": [], "meshlets": [], "meshes": [], "lods": []}
    out = {}
    for v in ("cubes", "tower"):
        p = d / f"{v}.gltf"
        cube_sword.write(p, v)
        lod = [dict(RM.prepare(pr), material_slot=0) for pr in gltf.read(p)["primitives"]]
        desc, blob = RM.build([lod], [mat], dict(base), "test_" + v)
        out[f"rigid-{v}"] = [h(RM.encode(FIXTURE_TYPES, desc)), h(blob)]
    fp, l1 = d / "figure.glb", d / "figure_lod1.glb"
    rigged_figure.write(fp)
    rigged_figure.write(l1, lod1=True)
    target = S.Target.from_skeleton(K.from_gltf(Doc(fp)), SKELETON)
    docs = [gltf.read(x) for x in (fp, l1)]
    for doc in docs:
        S.bind(doc, target)
    lods = [[dict(RM.prepare(pr), material_slot=0) for pr in doc["primitives"]] for doc in docs]
    desc, blob = RM.build(lods, [mat], dict(base), "figure", [0, 0.05], skin={"hierarchy": target.guid, "hierarchyJointCount": len(target), "globalScale": 1.0})
    out["skinned-figure"] = [h(RM.encode(FIXTURE_TYPES, desc)), h(blob)]
    tgt = TW.target_with()
    doc = gltf.read(TW.write_gltf(d / "q.gltf", {"Hood": [(0.0, 0.0, 0.02 * i) for i in range(4)], "Female": [(0.01 * i, 0.0, 0.0) for i in range(4)]}))
    S.attach(doc, tgt, 2)
    _, data, blob, _ = TW.build([doc], tgt)
    out["worn-morphs"] = [h(data), h(blob)]
    k = fakegame.build(d / "game", "client")
    try:
        rec = json.loads(json.dumps(fakegame.RECIPE))
        rec["steps"].append({"op": "model", "gltf": "src/golem.glb", "skeleton": "golem", "material": fakegame.MATERIAL, "name": "golem", "stem": "golem"})
        rp = fakegame.write_mod(d / "mod", rec)
        r = recipe.run(rp, k, fakegame.types(), update_manifest=True)
        assert r["ok"], r
        for f in sorted((rp.parent / "cooked").iterdir()):
            if not f.name.startswith("cook-report"):
                out["recipe/" + f.name] = h(f.read_bytes())
    finally:
        k.close()
    return out


class NonPhysicsUnchanged(unittest.TestCase):
    def test_models_without_physics_cook_to_the_same_bytes(self):
        with tempfile.TemporaryDirectory() as d:
            got = nonphysics_hashes(Path(d))
        self.assertEqual(got, NONPHYSICS)


class Recipes(unittest.TestCase):
    """assets/physics.json and the "experimental" key through cook.json (recipe.run), against the made-up game."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.types = fakegame.types()
        cls.client = fakegame.build(cls.dir / "client", "client", cls.types)
        cls.server = fakegame.build(cls.dir / "server", "server", cls.types)

    @classmethod
    def tearDownClass(cls):
        cls.client.close()
        cls.server.close()
        cls.tmp.cleanup()

    def mod(self, name, physics_doc, experimental=("modelcook.cloth",), steps_=None):
        rec = {"format": 1, "tool": "modelcook", "inputs": {}, "steps": steps_ if steps_ is not None else [
            {"op": "model", "gltf": "src/banner.gltf", "material": fakegame.MATERIAL, "guid": NEW, "mode": "new", "stem": "banner"}]}
        if experimental is not None:
            rec["experimental"] = list(experimental)
        rp = fakegame.write_mod(self.dir / name, rec)
        (rp.parent / "src" / "banner.gltf").write_bytes((BANNER / "banner.gltf").read_bytes())
        if physics_doc is not None:
            (rp.parent / "physics.json").write_text(json.dumps(physics_doc))
        return rp

    BANNER_PHYSICS = {"format": 1, "models": {"assets/src/banner.gltf": {"cloth": [{"mesh": 0, "primitive": 0, "pin": [1, 1, 0, 0], "massKg": 0.3}]}}}

    def test_cooks_with_the_key_and_says_so_once(self):
        rp = self.mod("on", self.BANNER_PHYSICS)
        r = recipe.run(rp, self.client, self.types, update_manifest=True)
        self.assertTrue(r["ok"], json.dumps(r["steps"])[:2000])
        self.assertEqual(r["features"], ["modelcook.cloth"])
        self.assertEqual(len(r["warnings"]), 1)
        self.assertIn("modelcook.cloth (game test pending: 16e-cc16e)", r["warnings"][0])
        d = Decoder(self.types).decode("keen::RenderModel", (rp.parent / "cooked" / "banner.bin").read_bytes())
        self.assertEqual(d["cloth"]["meshes"][0]["data"]["fabric"]["anchors"], [0, 1, 0, 1])
        self.assertIn("physics.json", r["sources"])
        self.assertFalse(recipe.needs_cook(rp))
        (rp.parent / "physics.json").write_text(json.dumps({"format": 1, "models": {"assets/src/banner.gltf": {"cloth": [{"mesh": 0, "primitive": 0, "pin": [1, 1, 0, 0], "massKg": 0.4}]}}}))
        self.assertTrue(recipe.needs_cook(rp), "a changed physics.json means a new cook")

    def test_materials_made_from_the_gltf_keep_the_cloth(self):
        """A step with "materials": "gltf" (what a body's and a look's steps do) once lost its model's physics.json entry (16e S4: a loop
        variable of the material blocks took the model's key)."""
        rp = self.mod("gltf-materials", self.BANNER_PHYSICS, steps_=[{"op": "model", "gltf": "src/banner.gltf", "materials": "gltf", "guid": NEW, "mode": "new", "stem": "banner"}])
        doc = json.loads((rp.parent / "src" / "banner.gltf").read_text())
        doc["materials"] = [{"name": "flag"}]
        doc["meshes"][0]["primitives"][0]["material"] = 0
        (rp.parent / "src" / "banner.gltf").write_text(json.dumps(doc))
        r = recipe.run(rp, self.client, self.types, update_manifest=True)
        self.assertTrue(r["ok"], json.dumps(r["steps"])[:2000])
        self.assertEqual(r["steps"][0]["cloth"]["meshes"][0]["particles"], 4)

    def test_without_the_key_the_build_is_refused_on_both_sides(self):
        for side, k in (("client", self.client), ("server", self.server)):
            with self.subTest(side=side):
                rp = self.mod(f"off-{side}", self.BANNER_PHYSICS, experimental=None)
                with self.assertRaisesRegex(recipe.RecipeError, r'^assets/physics.json: models\["assets/src/banner.gltf"\]\.cloth: needs cook feature modelcook.cloth'):
                    recipe.run(rp, k, self.types, update_manifest=True)
                self.assertFalse((rp.parent / "cooked" / "banner.bin").exists())
                self.assertFalse((rp.parent / "assets.json").exists(), "a refused build writes nothing")

    def test_a_server_skips_the_model_and_its_physics(self):
        rp = self.mod("server", self.BANNER_PHYSICS)
        r = recipe.run(rp, self.server, self.types, update_manifest=True)
        self.assertTrue(r["ok"])
        self.assertTrue(r["steps"][0]["skipped"])

    def test_physics_no_step_cooks_is_refused(self):
        doc = {"format": 1, "models": {"assets/src/other.gltf": self.BANNER_PHYSICS["models"]["assets/src/banner.gltf"]}}
        rp = self.mod("unused", doc)
        with self.assertRaisesRegex(recipe.RecipeError, r'models\["assets/src/other.gltf"\]: no model step of this mod cooks assets/src/other.gltf'):
            recipe.run(rp, self.client, self.types, update_manifest=True)

    def test_unknown_feature_name(self):
        rp = self.mod("typo", None, experimental=("modelcook.clth",))
        with self.assertRaisesRegex(recipe.RecipeError, r"experimental\[0\]: 'modelcook.clth' is not a cook feature"):
            recipe.load(rp)

    def test_a_bad_sidecar_is_refused_by_field(self):
        rp = self.mod("bad", {"format": 1, "models": {"assets/src/banner.gltf": {"cloth": [{"mesh": 0, "primitive": 0, "pin": [1, 1, 2, 0]}]}}})
        with self.assertRaisesRegex(recipe.RecipeError, r'^assets/physics.json: models\["assets/src/banner.gltf"\]\.cloth\[0\]\.pin\[2\]: 2.0 is outside \[0, 1\]'):
            recipe.run(rp, self.client, self.types, update_manifest=True)

    def body_mod(self, name, blocks, experimental=("modelcook.cloth",), physics_doc=None):
        """A mod whose blocks/npc.json has bodies on the skinned cape (assets/src/cape.gltf, and cloak.gltf, the same file)."""
        rp = self.mod(name, physics_doc, experimental=experimental, steps_=[])
        skinned_cape(rp.parent / "src" / "cape.gltf")
        skinned_cape(rp.parent / "src" / "cloak.gltf")
        (rp.parent.parent / "blocks").mkdir(exist_ok=True)
        (rp.parent.parent / "blocks" / "npc.json").write_text(json.dumps(blocks))
        return rp

    CAPE_CLOTH = [{"mesh": 0, "primitive": 0, "pin": [1, 1, 1, 0, 0, 0, 0, 0, 0], "massKg": 0.5}]

    def test_a_body_blocks_cloth_is_cooked_into_its_model(self):
        rp = self.body_mod("body", {"bodies": {"caped": {"model": "assets/src/cape.gltf", "cloth": self.CAPE_CLOTH}}})
        r = recipe.run(rp, self.client, self.types, update_manifest=True)
        self.assertTrue(r["ok"], json.dumps(r["steps"])[:2000])
        cooked = rp.parent / "cooked"
        side = json.loads((cooked / "assets_src_cape.model.json").read_text())
        want = PH.spec_hash(PH.parse_model("assets/src/cape.gltf", {"cloth": self.CAPE_CLOTH}, "blocks/npc.json"))
        self.assertEqual(side["physics"], {"spec": want, "cloth": 1, "chains": 0})
        d = Decoder(self.types).decode("keen::RenderModel", (cooked / "assets_src_cape.bin").read_bytes())
        self.assertEqual(d["cloth"]["meshes"][0]["data"]["fabric"]["numParticles"], 9)
        self.assertTrue(d["cloth"]["hasSkinning"])
        self.assertIn("../blocks/npc.json", r["sources"])
        # a body without cloth: its model's side file says nothing of physics (as before)
        rp2 = self.body_mod("body-plain", {"bodies": {"plain": {"model": "assets/src/cape.gltf"}}}, experimental=None)
        self.assertTrue(recipe.run(rp2, self.client, self.types, update_manifest=True)["ok"])
        self.assertNotIn("physics", json.loads((rp2.parent / "cooked" / "assets_src_cape.model.json").read_text()))

    def test_a_body_blocks_cloth_needs_the_key(self):
        rp = self.body_mod("body-off", {"bodies": {"caped": {"model": "assets/src/cape.gltf", "cloth": self.CAPE_CLOTH}}}, experimental=None)
        with self.assertRaisesRegex(recipe.RecipeError, r"^blocks/npc.json: body caped\.cloth: needs cook feature modelcook.cloth"):
            recipe.run(rp, self.client, self.types, update_manifest=True)

    def test_one_model_physics_from_one_place(self):
        both = {"format": 1, "models": {"assets/src/cape.gltf": {"cloth": self.CAPE_CLOTH}}}
        rp = self.body_mod("body-twice", {"bodies": {"caped": {"model": "assets/src/cape.gltf", "cloth": self.CAPE_CLOTH}}}, physics_doc=both)
        with self.assertRaisesRegex(recipe.RecipeError, r"^blocks/npc.json: body caped: 'assets/src/cape.gltf' already has physics from assets/physics.json"):
            recipe.run(rp, self.client, self.types, update_manifest=True)
        rp = self.body_mod("body-two-bodies", {"bodies": {"a": {"model": "assets/src/cape.gltf", "cloth": self.CAPE_CLOTH},
                                                          "b": {"model": "assets/src/cape.gltf", "chains": [{"rootJoint": "upper", "tipJoint": "lower"}]}}})
        with self.assertRaisesRegex(recipe.RecipeError, r"body b: its model assets/src/cape.gltf has cloth and chains from blocks/npc.json: body a already"):
            recipe.load(rp)

    def test_a_bad_body_cloth_is_refused_by_its_block_path(self):
        rp = self.body_mod("body-bad", {"creatures": {"golem": {"body": {"model": "assets/src/cape.gltf", "cloth": [{"mesh": 0, "primitive": 0, "pin": [1, 2]}]}}}})
        with self.assertRaisesRegex(recipe.RecipeError, r"^blocks/npc.json: body golem.body\.cloth\[0\]\.pin\[1\]: 2.0 is outside \[0, 1\]"):
            recipe.run(rp, self.client, self.types, update_manifest=True)

    def test_a_recipe_step_for_the_body_model_keeps_its_cloth(self):
        rp = self.body_mod("body-step", {"bodies": {"caped": {"model": "assets/src/cape.gltf", "cloth": self.CAPE_CLOTH}}})
        doc = json.loads(rp.read_text())
        doc["steps"] = [{"tool": "animcook", "op": "skeleton", "name": "cape", "gltf": "src/cape.gltf"},
                        {"op": "model", "gltf": "src/cape.gltf", "skeleton": "cape", "material": fakegame.MATERIAL, "name": "assets/src/cape", "stem": "mine"}]
        rp.write_text(json.dumps(doc))
        r = recipe.run(rp, self.client, self.types, update_manifest=True)
        self.assertTrue(r["ok"], json.dumps(r["steps"])[:2000])
        self.assertEqual(json.loads((rp.parent / "cooked" / "mine.model.json").read_text())["physics"]["cloth"], 1)

    def test_worn_path_carries_cloth(self):
        """G2: a worn look's model step reads physics.json by the look's model path, through the same model step."""
        rp = self.mod("worn", None, steps_=[
            {"tool": "animcook", "op": "skeleton", "name": "golem", "gltf": "src/golem.glb"},
            {"op": "worn", "model": "assets/src/flag.gltf", "skeleton": "golem", "attach": "j1", "materials": {"*": fakegame.MATERIAL}}])
        pos, idx, uvs = grid(2, 1, size=0.25)
        write_gltf(rp.parent / "src" / "flag.gltf", [[{"positions": pos, "indices": idx, "uvs": uvs}]], [{"mesh": 0}])
        (rp.parent / "physics.json").write_text(json.dumps({"format": 1, "models": {"assets/src/flag.gltf": {"cloth": [{"mesh": 0, "primitive": 0, "pin": top_pinned(2, 1)}]}}}))
        r = recipe.run(rp, self.client, self.types, update_manifest=True)
        self.assertTrue(r["ok"], json.dumps(r["steps"])[:2000])
        bins = [f for f in (rp.parent / "cooked").glob("*.bin")]
        self.assertEqual(len(bins), 1)
        d = Decoder(self.types).decode("keen::RenderModel", bins[0].read_bytes())
        q = d["cloth"]["meshes"][0]["data"]
        self.assertEqual({tuple(s["jointIndices"]) for s in q["simulationSkinningData"]}, {(1, 0, 0, 0)})
        self.assertTrue(d["cloth"]["hasSkinning"])


if __name__ == "__main__":
    unittest.main()
