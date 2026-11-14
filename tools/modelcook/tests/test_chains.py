# SPDX-License-Identifier: MIT
"""Bone chains (cook/chains.py) without game files: skinned rigs with a pendant are cooked through steps.model and cook.json against the
made-up game of tests/fakegame.py and decoded with tools/kfc. 2- and 8-joint chains round-trip, 9 is refused, the packing's signed codes, the
palette (sorted ordinary joints and anchors, then reserved slots in chain order, never slot 0), the GUID rule, extents, fixed tuning, every
refusal of chapter 16e 2.2 and 5.3, and the canonical frame checked against a port of the research's evidence/chain_semantics.py.

    python3 -m unittest discover -s tools/modelcook/tests -p test_chains.py
"""
import json
import math
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent.parent
sys.path[:0] = [str(HERE), str(HERE.parent), str(TOOLS / "kfc"), str(TOOLS / "kfc" / "tests"), str(TOOLS / "animcook")]

import fakegame  # noqa: E402
from anicook import skeleton as K  # noqa: E402
from anicook.gltfdoc import Doc  # noqa: E402
from anicook.graphgen import derive  # noqa: E402
from cook import chains as CH, physics as PH, recipe, rendermodel as RM, skin as S, steps  # noqa: E402
from kfc.blob import Decoder  # noqa: E402
from kfc.refl import TypeDb  # noqa: E402
from test_cloth_model import write_gltf  # noqa: E402

NEW = "77777777-0000-4000-8000-000000000007"
SKELETON = "aaaaaaaa-0000-4000-8000-000000000002"
CHAINS_ON = frozenset({"modelcook.chains2"})


def quat(axis, deg):
    s = math.sin(math.radians(deg) / 2)
    n = math.sqrt(sum(a * a for a in axis))
    return [axis[0] / n * s, axis[1] / n * s, axis[2] / n * s, math.cos(math.radians(deg) / 2)]


def rig(path, links=3, extra=(), scale=None, side_child=False, second=None, morph=False, link=None):
    """A skinned glTF: root -> spine (y 1.5) -> p0 .. p<links-1> hanging down and turning, plus `extra` joints under spine (name list), and with
    second=n a second pendant q0..q<n-1> under spine declared before the first in the file (its joints get lower skeleton indices). Every joint
    carries the vertices of a small quad at its place; vertices of spine and root too. -> (joint names in skin order)."""
    nodes = [{"name": "root", "children": [1]}, {"name": "spine", "translation": [0, 1.5, 0], "rotation": quat((0, 1, 0), 20), "children": []}]
    names = ["root", "spine"]

    def hang(prefix, n, under, first_offset, turn):
        idx = []
        parent = under
        for i in range(n):
            ni = len(nodes)
            step = (link(i) if link else None) or [0.03 * i, -0.25, 0.02 * i]
            node = {"name": f"{prefix}{i}", "translation": first_offset if i == 0 else step, "rotation": quat(turn, 12 + 7 * i), "children": []}
            if scale and i == 1:
                node["scale"] = scale
            nodes.append(node)
            nodes[parent]["children"].append(ni)
            names.append(node["name"])
            idx.append(ni)
            parent = ni
        return idx

    if second:
        hang("q", second, 1, [-0.2, -0.1, 0.05], (0, 0, 1))
    hang("p", links, 1, [0.15, -0.1, 0.1], (1, 0, 0.3))
    for e in extra:
        ni = len(nodes)
        nodes.append({"name": e, "translation": [0, 0.2, 0], "children": []})
        nodes[1]["children"].append(ni)
        names.append(e)
    if side_child:
        ni = len(nodes)
        nodes.append({"name": "side", "translation": [0.1, 0, 0], "children": []})
        nodes[names.index("p1")]["children"].append(ni)
        names.append("side")
    # world matrices (rows) to place vertices and inverse binds
    import cook.gltf as G
    world = G.world_matrices(nodes)
    joints = list(range(len(nodes)))
    pos, idx, js, ws = [], [], [], []
    for j in joints:
        w = world[j]
        c = (w[0][3], w[1][3], w[2][3])
        base = len(pos)
        for dx, dy in ((-0.04, -0.04), (0.04, -0.04), (-0.04, 0.04), (0.04, 0.04)):
            pos.append((c[0] + dx, c[1] + dy, c[2]))
            js.append((j, 0, 0, 0))
            ws.append((1.0, 0, 0, 0))
        idx += [base, base + 1, base + 2, base + 2, base + 1, base + 3]
    ibm = []
    for w in world:
        r = [[w[i][k] for k in range(3)] for i in range(3)]
        t = [w[i][3] for i in range(3)]
        # inverse of an affine with possible scale: invert the 3x3
        det = (r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) + r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]))
        ri = [[(r[(j + 1) % 3][(i + 1) % 3] * r[(j + 2) % 3][(i + 2) % 3] - r[(j + 1) % 3][(i + 2) % 3] * r[(j + 2) % 3][(i + 1) % 3]) / det for j in range(3)] for i in range(3)]
        ti = [-sum(ri[i][k] * t[k] for k in range(3)) for i in range(3)]
        ibm.append([ri[0] + [ti[0]], ri[1] + [ti[1]], ri[2] + [ti[2]], [0, 0, 0, 1]])
    nodes.append({"name": "body", "mesh": 0, "skin": 0})
    prim = {"positions": pos, "indices": idx, "joints": js, "weights": ws}
    if morph:
        prim["morphs"] = {"Female": [(0, 0.001, 0)] * len(pos)}
    write_gltf(path, [[prim]], nodes, skins=[{"joints": joints, "ibm": ibm}])
    return names


def physics(model, *chains, **kw):
    return PH.parse({"format": 1, "models": {model: {"chains": [dict(rootJoint=r, tipJoint=t, **kw) for r, t in chains]}}}).get(model)


class Game:
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

    def cook(self, path, phys, features=CHAINS_ON, mod="chain-mod", target=None, types=None, **kw):
        target = target or S.Target.from_skeleton(K.from_gltf(Doc(path)), SKELETON)
        files, entries, report = steps.model(self.k, types or self.types, [str(path)], None, guid=NEW, mode="new", material=fakegame.MATERIAL,
                                             target=target, physics=phys, features=features, mod=mod, **kw)
        data, blob = files[report["bin"]], files[report["blob"]]
        return Decoder(self.types).decode("keen::RenderModel", data), data, blob, report, target


# ---- a port of the research's evidence/chain_semantics.py (global anchor, shortest arc): independent of cook/chains.py's math ----

def _R(m):
    return [[m["rot"][c][r] for c in "xyz"] for r in "xyz"]


def _mv(a, v):
    return [sum(a[i][j] * v[j] for j in range(3)) for i in range(3)]


def _mm(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def _tr(a):
    return [list(x) for x in zip(*a)]


def _rot_dir(v):
    a = [0, -1.0, 0]
    n = math.sqrt(sum(x * x for x in v))
    b = [x / n for x in v]
    c = sum(x * y for x, y in zip(a, b))
    if c < -0.999999:
        return [[-1.0, 0, 0], [0, -1.0, 0], [0, 0, 1.0]]
    w = [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
    K_ = [[0, -w[2], w[1]], [w[2], 0, -w[0]], [-w[1], w[0], 0]]
    K2 = _mm(K_, K_)
    I_ = [[1.0, 0, 0], [0, 1.0, 0], [0, 0, 1.0]]
    return [[I_[i][j] + K_[i][j] + K2[i][j] / (1 + c) for j in range(3)] for i in range(3)]


def semantics_positions(c, i):
    """chain_semantics.py's global-shortest candidate for chain i of decoded ChainData c."""
    ch = c["chains2"][i]
    a = c["anchorDefaultPose"][i]
    r, p = _R(a), [a["pos"][k] for k in "xyz"]
    pts = []
    for m in c["modelMatrices"][ch["jointBeginIndex"]:ch["jointEndIndex"]]:
        p = [x + y for x, y in zip(p, _mv(r, [m["pos"][k] for k in "xyz"]))]
        r = _mm(r, _R(m))
        pts.append(p)
    canonical = _rot_dir([x - y for x, y in zip(pts[1], pts[0])])
    return [_mv(_tr(canonical), [x - y for x, y in zip(v, pts[0])]) for v in pts[1:]]


def semantics_codes(v, extent):
    q = extent / 511000.0
    return [max(-512, min(511, math.floor(x / q + .5) if x >= 0 else math.ceil(x / q - .5))) for x in v]


class Packing(unittest.TestCase):
    def test_codes_and_bits(self):
        self.assertEqual(CH.pack((0.0, 0.0, 0.0), 100), 0)
        self.assertEqual(CH.codes(CH.pack((0.1, -0.1, 0.0), 100)), (511, -511, 0))
        # saturation: beyond the extent the code stays at -512 / 511, never wraps
        self.assertEqual(CH.codes(CH.pack((1.0, -1.0, 0.0), 100)), (511, -512, 0))
        w = CH.pack((-1.0, 0.0, 1.0), 100)
        self.assertEqual(w & 0x3FF, 0x200)  # -512, two's complement in 10 bits
        self.assertEqual((w >> 20) & 0x3FF, 0x1FF)
        self.assertEqual(CH.unpack(w, 100), (-512 * 100 / 511000, 0.0, 511 * 100 / 511000))
        # round half away from zero
        half = 0.5 * 100 / 511000
        self.assertEqual(CH.codes(CH.pack((half, -half, 0.0), 100))[:2], (1, -1))

    def test_shortest_arc(self):
        for d in [(0, -1, 0), (1, 0, 0), (0, 0, 1), (0.3, -0.8, 0.2), (0, 1, 0), (1e-9, 1, 0)]:
            n = math.sqrt(sum(x * x for x in d))
            d = tuple(x / n for x in d)
            r = CH.quat_matrix(CH.shortest_arc(d))
            got = [sum(r[i][k] * (0, -1, 0)[k] for k in range(3)) for i in range(3)]
            for a, b in zip(got, d):
                self.assertAlmostEqual(a, b, places=6)
        self.assertEqual(CH.shortest_arc((0.0, 1.0, 0.0)), (0.0, 0.0, -1.0, 0.0))  # the client's axis for the half turn


class Chains(Game, unittest.TestCase):
    def test_a_three_joint_pendant(self):
        path = self.dir / "pendant.gltf"
        names = rig(path, links=3)
        phys = physics("models/pendant.gltf", ("p0", "p2"), particleMassKg=0.5, stiffness=20000, bendStiffness=7, damping=0.05, particleRadius=0.0125)
        d, data, blob, r, target = self.cook(path, phys)
        self.assertTrue(r["ok"], json.dumps({k: v for k, v in r["checks"].items() if not v}))
        for k in ("chain_lists_agree", "chain_rest_within_a_code_step", "joint_map_consistent", "every_map_entry_carries_weight"):
            self.assertIs(r["checks"][k], True, k)
        c = d["chain"]
        ch = c["chains2"][0]
        cfg = ch["config"]
        # palette: root and spine carry weight and spine anchors (sorted), then p0 p1 p2's reserved slots holding 0
        spine, p0, p1, p2 = (names.index(n) for n in ("spine", "p0", "p1", "p2"))
        self.assertEqual(d["skinningMap"], [0, spine, 0, 0, 0])
        self.assertEqual(d["jointCount"], 5)
        self.assertEqual(list(c["joints"]), [2, 3, 4])
        self.assertEqual(c["anchorHierarchyJoints"], [spine])
        self.assertEqual((ch["jointBeginIndex"], ch["jointEndIndex"], cfg["count"], len(cfg["restPosition"])), (0, 3, 3, 2))
        # the render skin: a vertex on p1 points at p1's reserved slot, one on spine at spine's sorted slot
        st = d["skinningData"]["start"]
        self.assertEqual(blob[st + 8 * 4 * p1], 3)
        self.assertEqual(blob[st + 8 * 4 * spine], 1)
        # tuning: asked for, and fixed
        self.assertEqual((cfg["stiffness"], cfg["bendStiffness"], cfg["inverseMass"], cfg["damping"], cfg["particleRadius"]), (20000.0, 7.0, 2.0, struct.unpack("<f", struct.pack("<f", 0.05))[0], 13))
        self.assertEqual((cfg["stretchLimit"], cfg["compressionLimit"], cfg["linearInertiaHorizontal"], cfg["linearInertiaVertical"], cfg["bendStiffnessTipScale"], cfg["gravityScale"]),
                         (1.0, struct.unpack("<f", struct.pack("<f", 0.7))[0], 6554, 6554, 255, 100))
        self.assertEqual(cfg["angleLimitCos"], [0] * 7)
        self.assertEqual(cfg["motionRadius"], [1.0, 2.0, 3.0, 3.0, 3.0, 3.0, 3.0])
        self.assertTrue(ch["propagateRotation"])
        self.assertEqual((ch["colliderIndex"], len(c["chainColliders"])), (0, 1))
        self.assertEqual(c["chainColliders"][0], {"hasSkinning": False, "spheres": [], "capsules": []})
        for k in ("chains", "configs", "anchorRestPosition", "restPosition", "constraintJoints", "constraints"):
            self.assertEqual(c[k], [], k)
        # GUID: the mod's namespace, model-chain:, the canonical JSON of [model, root, tip]
        self.assertEqual(ch["guid"], derive("chain-mod", "model-chain", '["models/pendant.gltf","p0","p2"]'))
        # extents: ceil(1000 E) of the canonical rest shape, ceil(2000 L) of the links
        b = [target.bind[j] for j in (p0, p1, p2)]
        reach = sum(math.dist(CH.pos(b[i]), CH.pos(b[i - 1])) for i in (1, 2))
        self.assertEqual(cfg["extentMaxAllowedDist"], math.ceil(2000 * reach))
        exp = semantics_positions(c, 0)
        self.assertEqual(cfg["extentRestPosition"], math.ceil(1000 * max(abs(x) for v in exp for x in v)))
        # the frame: the research's own reconstruction of the stored matrices gives the packed codes, code for code
        self.assertEqual([CH.codes(w) for w in cfg["restPosition"]], [tuple(semantics_codes(v, cfg["extentRestPosition"])) for v in exp])
        # and the stored local matrices rebuild the bind pose of the skeleton (float32)
        rebuilt = CH.matrix_of(c["anchorDefaultPose"][0])
        for k, j in enumerate((p0, p1, p2)):
            rebuilt = CH.mul(rebuilt, CH.matrix_of(c["modelMatrices"][k]))
            for row in range(3):
                for col in range(4):
                    self.assertAlmostEqual(rebuilt[row][col], target.bind[j][row][col], places=5)
            inv = CH.matrix_of(c["skinToJointMatrix"][k])
            ident = CH.mul(inv, target.bind[j])
            for row in range(3):
                for col in range(4):
                    self.assertAlmostEqual(ident[row][col], 1.0 if row == col else 0.0, places=5)
        self.assertEqual(r["chains"][0]["slots"], [2, 3, 4])

    def test_two_and_eight_joints_and_nine_refused(self):
        for links in (2, 8):
            with self.subTest(links=links):
                path = self.dir / f"p{links}.gltf"
                rig(path, links=links)
                d, _, _, r, _ = self.cook(path, physics(f"m{links}.gltf", ("p0", f"p{links - 1}")))
                self.assertTrue(r["ok"], json.dumps({k: v for k, v in r["checks"].items() if not v}))
                cfg = d["chain"]["chains2"][0]["config"]
                self.assertEqual((cfg["count"], len(cfg["restPosition"]), len(d["chain"]["joints"])), (links, links - 1, links))

    def test_nine_joints_overrun_the_engine_and_are_refused(self):
        path = self.dir / "p9.gltf"
        rig(path, links=9)
        with self.assertRaisesRegex(steps.StepError, r"chains\[0\]: 'p0' to 'p8' is 9 joints: a chain has at most 8 .*overrun"):
            self.cook(path, physics("m9.gltf", ("p0", "p8")))
        rig(path, links=12)
        with self.assertRaisesRegex(steps.StepError, r"'p0' to 'p11' is 12 joints"):
            self.cook(path, physics("m9.gltf", ("p0", "p11")))

    def test_the_palette_is_not_coalesced_and_slot_0_never_swings(self):
        """Two chains: q (lower skeleton indices, declared second) and p (declared first). Reserved slots follow the declaration, not the skeleton."""
        path = self.dir / "two.gltf"
        names = rig(path, links=3, second=2)
        d, _, _, r, _ = self.cook(path, physics("two.gltf", ("p0", "p2"), ("q0", "q1")))
        self.assertTrue(r["ok"], json.dumps({k: v for k, v in r["checks"].items() if not v}))
        ordinary = [names.index("root"), names.index("spine")]
        self.assertEqual(d["skinningMap"], ordinary + [0] * 5)
        self.assertEqual(list(d["chain"]["joints"]), [2, 3, 4, 5, 6])  # p0 p1 p2, then q0 q1
        self.assertEqual([(c["jointBeginIndex"], c["jointEndIndex"]) for c in d["chain"]["chains2"]], [(0, 3), (3, 5)])
        self.assertEqual([c["colliderIndex"] for c in d["chain"]["chains2"]], [0, 1])  # one (empty) collider per chain, as the game's
        self.assertEqual(len(d["chain"]["chainColliders"]), 2)
        self.assertNotIn(0, d["chain"]["joints"])
        self.assertLess(names.index("q0"), names.index("p0"))

    def test_an_unweighted_anchor_is_in_the_palette(self):
        path = self.dir / "anchor.gltf"
        rig(path, links=2)
        doc = json.loads(path.read_text())
        # take the spine's vertices onto the root: spine anchors the chain and carries no weight
        import base64
        raw = bytearray(base64.b64decode(doc["buffers"][0]["uri"].split(",", 1)[1]))
        jacc = doc["accessors"][doc["meshes"][0]["primitives"][0]["attributes"]["JOINTS_0"]]
        at = doc["bufferViews"][jacc["bufferView"]]["byteOffset"]
        for v in range(4, 8):
            raw[at + 4 * v] = 0
        doc["buffers"][0]["uri"] = "data:application/octet-stream;base64," + base64.b64encode(bytes(raw)).decode()
        path.write_text(json.dumps(doc))
        d, _, _, r, _ = self.cook(path, physics("anchor.gltf", ("p0", "p1")))
        self.assertTrue(r["ok"], json.dumps({k: v for k, v in r["checks"].items() if not v}))
        self.assertEqual(d["skinningMap"], [0, 1, 0, 0])

    def test_lods_share_the_palette(self):
        path = self.dir / "lod0.gltf"
        rig(path, links=3)
        target = S.Target.from_skeleton(K.from_gltf(Doc(path)), SKELETON)
        files, _, r = steps.model(self.k, self.types, [str(path), str(path)], None, guid=NEW, mode="new", material=fakegame.MATERIAL, target=target,
                                  physics=physics("lod.gltf", ("p0", "p2")), features=CHAINS_ON, mod="m", pixel_sizes=[0, 0.05])
        self.assertTrue(r["ok"], json.dumps({k: v for k, v in r["checks"].items() if not v}))
        d = Decoder(self.types).decode("keen::RenderModel", files[r["bin"]])
        self.assertEqual(len(d["lods"]), 2)


class Refusals(Game, unittest.TestCase):
    def refused(self, pattern, path, phys, **kw):
        with self.assertRaisesRegex(steps.StepError, pattern):
            self.cook(path, phys, **kw)

    def test_the_skeleton_rules(self):
        path = self.dir / "r.gltf"
        rig(path, links=3, extra=("tail",))
        self.refused(r"chains\[0\]\.rootJoint: 'nope' is not a joint", path, physics("r.gltf", ("nope", "p2")))
        self.refused(r"chains\[0\]\.tipJoint: 'tail' is not below 'p0'", path, physics("r.gltf", ("p0", "tail")))
        self.refused(r"chains\[0\]\.rootJoint: 'root' is the skeleton's root", path, physics("r.gltf", ("root", "spine")))
        self.refused(r"chains\[1\]: joint 'p1' already swings in .*chains\[0\]", path, physics("r.gltf", ("p0", "p2"), ("p1", "p2")))
        self.refused(r"chains\[0\]: joint 'p1' of the chain has 'p2' below it outside the chain", path, physics("r.gltf", ("p0", "p1")))
        self.refused(r"chains\[0\]: joint 'spine' of the chain has .* below it outside the chain", path, physics("r.gltf", ("spine", "p2")))
        rig(path, links=3, side_child=True)
        self.refused(r"joint 'p1' of the chain has 'side' below it", path, physics("r.gltf", ("p0", "p2")))

    def test_an_anchor_does_not_swing(self):
        path = self.dir / "a.gltf"
        rig(path, links=4)
        self.refused(r"chains\[1\]: its anchor 'p1' swings in .*chains\[0\]", path, physics("a.gltf", ("p0", "p1"), ("p2", "p3")))

    def test_rigid_bones(self):
        path = self.dir / "s.gltf"
        rig(path, links=3, scale=[1.5, 1.5, 1.5])
        self.refused(r"joint 'p1''s bind pose is not rigid: its axis x has length 1.5", path, physics("s.gltf", ("p0", "p2")))

    def test_reach_and_zero_links(self):
        path = self.dir / "z.gltf"
        rig(path, links=3, link=lambda i: [0, 0, 0] if i == 1 else None)
        self.refused(r"joints 'p0' and 'p1' are at one place", path, physics("z.gltf", ("p0", "p2")))
        rig(path, links=3, link=lambda i: [0, -40, 0] if i == 1 else None)
        self.refused(r"the chain is 40\.\d+ m long: at most 32\.7675 m", path, physics("z.gltf", ("p0", "p2")))

    def test_what_a_chain_needs_of_its_model(self):
        path = self.dir / "n.gltf"
        rig(path, links=3)
        phys = physics("n.gltf", ("p0", "p2"))
        self.refused(r"\.chains: needs cook feature modelcook.chains2, which is off \(game test pending: 16e-cc16e\)", path, phys, features=frozenset())
        game = S.Target(SKELETON, ["root", "spine", "p0", "p1", "p2"], S.Target.from_skeleton(K.from_gltf(Doc(path)), SKELETON).bind, "game")
        self.refused(r"\.chains: skeleton .* is the game's", path, phys, target=game)
        flat = write_gltf(self.dir / "flat.gltf", [[{"positions": [(0, 0, 0), (1, 0, 0), (0, 1, 0)], "indices": [0, 1, 2]}]], [{"mesh": 0}])
        self.refused(r"\.chains: the model is attached to one joint", flat, phys, attach="j1", target=S.Target.from_skeleton(K.from_gltf(Doc(path)), SKELETON))
        self.refused(r"\.chains: the model's globalScale is 2: chains need 1", path, phys, global_scale=2.0)
        self.refused(r"chains\[0\]: joint 'p1' swings in this chain and clip 'wave' of src/pendant.gltf animates it", path, phys,
                     animated={"p1": ["clip 'wave' of src/pendant.gltf"]})
        with self.assertRaisesRegex(steps.StepError, r"\.chains: bone chains swing joints of the model's skeleton: the model is not skinned"):
            steps.model(self.k, self.types, [str(flat)], None, guid=NEW, mode="new", material=fakegame.MATERIAL, physics=phys, features=CHAINS_ON, mod="m")
        with self.assertRaisesRegex(steps.StepError, "mod's id"):
            self.cook(path, phys, mod=None)

    def test_layout_guard(self):
        import copy
        path = self.dir / "g.gltf"
        rig(path, links=3)
        dump = copy.deepcopy(self.types.dump)
        cfg = next(t for t in dump["types"] if t["name"] == "keen::RenderModelChainConfig2")
        next(f for f in cfg["fields"] if f["name"] == "count")["offset"] = 0x68
        moved = TypeDb(dump)
        self.assertEqual(RM.layout_problems(moved, "chains2"), ["keen::RenderModelChainConfig2.count is keen::uint16 at +0x68, the cook writes keen::uint16 at +0x6a"])
        self.refused(r"\.chains: this game's chain types differ .*count", path, physics("g.gltf", ("p0", "p2")), types=moved)


class Recipes(Game, unittest.TestCase):
    """Chains through cook.json: physics.json or a body block, and the clips of the same skeleton."""

    def mod(self, name, clips_anim=None, body=False, experimental=("modelcook.chains2",)):
        rec = {"format": 1, "tool": "modelcook", "inputs": {}, "steps": [
            {"tool": "animcook", "op": "skeleton", "name": "pendant", "gltf": "src/pendant.gltf"},
            {"op": "model", "gltf": "src/pendant.gltf", "skeleton": "pendant", "material": fakegame.MATERIAL, "name": "pendant", "stem": "pendant"}]}
        if clips_anim:
            rec["steps"].append({"tool": "animcook", "op": "clips", "skeleton": "pendant", "gltf": "src/pendant.gltf", "clips": {"wave": {"animation": clips_anim, "loop": True}}})
        if experimental is not None:
            rec["experimental"] = list(experimental)
        rp = fakegame.write_mod(self.dir / name, rec)
        rig(rp.parent / "src" / "pendant.gltf", links=3)
        if not body:
            (rp.parent / "physics.json").write_text(json.dumps({"format": 1, "models": {"assets/src/pendant.gltf": {"chains": [{"rootJoint": "p0", "tipJoint": "p2"}]}}}))
        return rp

    def add_animation(self, path, node_name, name="Wave"):
        doc = json.loads(path.read_text())
        import base64
        raw = bytearray(base64.b64decode(doc["buffers"][0]["uri"].split(",", 1)[1]))
        t_at = len(raw)
        raw += struct.pack("<2f", 0.0, 1.0)
        r_at = len(raw)
        raw += struct.pack("<8f", 0, 0, 0, 1, 0.1, 0, 0, 0.995)
        doc["bufferViews"] += [{"buffer": 0, "byteOffset": t_at, "byteLength": 8}, {"buffer": 0, "byteOffset": r_at, "byteLength": 32}]
        nv = len(doc["bufferViews"])
        doc["accessors"] += [{"bufferView": nv - 2, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
                             {"bufferView": nv - 1, "componentType": 5126, "count": 2, "type": "VEC4"}]
        na = len(doc["accessors"])
        node = next(i for i, n in enumerate(doc["nodes"]) if n.get("name") == node_name)
        doc["animations"] = [{"name": name, "samplers": [{"input": na - 2, "output": na - 1}], "channels": [{"sampler": 0, "target": {"node": node, "path": "rotation"}}]}]
        doc["buffers"][0] = {"byteLength": len(raw), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(raw)).decode()}
        path.write_text(json.dumps(doc))

    def test_cooks_from_physics_json(self):
        rp = self.mod("chains-on")
        r = recipe.run(rp, self.k, self.types, update_manifest=True)
        self.assertTrue(r["ok"], json.dumps(r["steps"])[:2000])
        d = Decoder(self.types).decode("keen::RenderModel", (rp.parent / "cooked" / "pendant.bin").read_bytes())
        self.assertEqual(d["chain"]["chains2"][0]["guid"], derive("fake-game-mod", "model-chain", '["assets/src/pendant.gltf","p0","p2"]'))
        side = json.loads((rp.parent / "cooked" / "pendant.model.json").read_text())
        self.assertEqual(side["physics"]["chains"], 1)
        self.assertEqual(side["joints"], d["skinningMap"])

    def test_an_animated_swinging_joint_is_refused(self):
        rp = self.mod("chains-anim", clips_anim="Wave")
        self.add_animation(rp.parent / "src" / "pendant.gltf", "p1")
        with self.assertRaisesRegex(recipe.RecipeError, r"chains\[0\]: joint 'p1' swings in this chain and clip 'Wave' of src/pendant.gltf animates it"):
            try:
                recipe.run(rp, self.k, self.types, update_manifest=True)
            except steps.StepError as e:
                raise recipe.RecipeError(str(e)) from e
        # the same clip moving the anchor is fine
        rp2 = self.mod("chains-anim-ok", clips_anim="Wave")
        self.add_animation(rp2.parent / "src" / "pendant.gltf", "spine")
        self.assertTrue(recipe.run(rp2, self.k, self.types, update_manifest=True)["ok"])

    def test_without_the_key(self):
        rp = self.mod("chains-off", experimental=None)
        with self.assertRaisesRegex(recipe.RecipeError, r'models\["assets/src/pendant.gltf"\]\.chains: needs cook feature modelcook.chains2'):
            recipe.run(rp, self.k, self.types, update_manifest=True)


if __name__ == "__main__":
    unittest.main()
