# SPDX-License-Identifier: MIT
"""The cloth fabric (cook/fabric.py) without game files: the banner (tests/fixtures/banner: chapter 16e's synthetic reference, its
expected-fabric.json written by the research probe fabric_v1.py) comes out as its golden, the array relations of chapter 16e 2.1 hold
on a seamed and a two-piece cloth, the same input gives the same bytes in two interpreters, and the reference contract tests of the
fabric (evidence/test_selected_contracts.py: banner masses and tethers, fractional pins, bad input) pass on the port.

    python3 -m unittest discover -s tools/modelcook/tests -p test_fabric.py
"""
import json
import os
import struct
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE.parent), str(HERE.parent.parent / "kfc")]

from cook import fabric as FB, gltf, physics as P  # noqa: E402
from kfc.blob import Decoder  # noqa: E402
from kfc.blobwrite import Encoder  # noqa: E402
from kfc.refl import TypeDb  # noqa: E402

TYPES = TypeDb.load(HERE / "fixtures" / "types-rendermodel.json")
BANNER = HERE / "fixtures" / "banner"
POS = [(0, 1, 0), (1, 1, 0), (0, 0, 0), (1, 0, 0)]
TRIS = [(0, 2, 1), (1, 2, 3)]


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def banner():
    """The banner as the cook reads it: its glTF and its sidecar."""
    prim = gltf.read(BANNER / "banner.gltf")["primitives"][0]
    c = P.load(BANNER / "physics.json").get("banner.gltf").cloth[0]
    mesh = P.physical_mesh(c, prim["positions"], prim["indices"])
    return c, prim, mesh, FB.cook(mesh.positions, mesh.triangles, mesh.pin, c.mass_kg)


def strip(cols, rows, x0=0.0):
    """A cols x rows grid of quads in the x/y plane (top row y = rows), its top row pinned. -> (positions, triangles, pin)."""
    pos = [(x0 + x, float(rows - y), 0.0) for y in range(rows + 1) for x in range(cols + 1)]
    w = cols + 1
    tris = []
    for y in range(rows):
        for x in range(cols):
            a, b, c, d = y * w + x, y * w + x + 1, (y + 1) * w + x, (y + 1) * w + x + 1
            tris += [(a, c, b), (b, c, d)]
    return pos, tris, [1.0 if i < w else 0.0 for i in range(len(pos))]


def relations(test, result, res=None):
    """Chapter 16e 2.1's relations of the cooked arrays (what all 320 of the game's cloth meshes satisfy)."""
    f = result["fabric"]
    n = f["numParticles"]
    test.assertEqual(len(result["inverseMasses"]), n)
    test.assertEqual(len(result["vertexWeights"]), n)
    test.assertEqual(len(f["indices"]), 2 * len(f["restValues"]))
    test.assertEqual(f["sets"][-1], len(f["restValues"]))
    test.assertEqual(f["sets"], sorted(f["sets"]))
    test.assertEqual(len(set(f["sets"])), len(f["sets"]), "a set is never empty")
    test.assertEqual(f["phaseIndices"], list(range(len(f["sets"]))))
    test.assertEqual(len(f["anchors"]), n)
    test.assertEqual(len(f["tetherLengths"]), n)
    test.assertEqual(f["stiffnessValues"], [])
    test.assertTrue(all(0 <= i < n for i in f["indices"] + f["anchors"] + f["triangles"]))
    prev = 0
    for end in f["sets"]:  # a batch is a set of independent constraints
        ix = f["indices"][2 * prev:2 * end]
        test.assertEqual(len(ix), len(set(ix)))
        prev = end
    for i, a in enumerate(f["anchors"]):  # an anchor is pinned; a pinned particle is its own anchor at 0
        test.assertEqual(result["inverseMasses"][a], 0.0)
        if result["inverseMasses"][i] == 0.0:
            test.assertEqual((a, f["tetherLengths"][i]), (i, 0.0))
    if res is not None:
        test.assertEqual(len(res["simulationPositions"]), n)
        test.assertEqual(len(res["simulationInvMasses"]), n)
        test.assertEqual(len(res["simulationVertexWeights"]), n)
        test.assertIn(len(res["simulationSkinningData"]), (0, n))
        test.assertEqual(res["simulationStiffnesses"], [])
        nr = len(res["renderPositions"])
        test.assertEqual(len(res["renderUvs"]), nr)
        test.assertEqual(len(res["renderSimulationVertexIndices"]), nr)
        test.assertTrue(all(0 <= i < n for i in res["renderSimulationVertexIndices"]))
        test.assertEqual(res["skinnedVertexIndices"], [i for i, m in enumerate(res["simulationInvMasses"]) if m == 0])
        test.assertEqual([i for t in res["simulationTriangles"] for i in (t["x"], t["y"], t["z"])], f["triangles"])
        test.assertTrue(all(0 <= t[k] < nr for t in res["renderTriangles"] for k in "xyz"))


class Golden(unittest.TestCase):
    def test_banner_is_the_golden(self):
        _, _, mesh, out = banner()
        expected = json.loads((BANNER / "expected-fabric.json").read_text())
        self.assertEqual(out, expected)
        relations(self, out)

    def test_banner_values_one_by_one(self):
        out = banner()[3]
        f = out["fabric"]
        self.assertEqual(f["indices"], [0, 1, 2, 3, 0, 2, 1, 3, 1, 2, 0, 3])  # 4 structural edges in 2 batches, 2 bending pairs: (1, 2) is a shared edge, (0, 3) the diagonal
        self.assertEqual(f["sets"], [2, 4, 5, 6])
        self.assertEqual(f["triangles"], [0, 2, 1, 1, 2, 3])


class Contracts(unittest.TestCase):
    """evidence/test_selected_contracts.py, the fabric part, against the port."""

    def test_banner_mass_tethers(self):
        d = FB.cook(POS, TRIS, [1, 1, 0, 0], 0.3)
        self.assertEqual(d["fabric"]["anchors"], [0, 1, 0, 1])
        self.assertEqual(d["fabric"]["tetherLengths"], [0, 0, 1, 1])
        self.assertEqual(d["vertexWeights"], [0, 0, 1, 1])
        self.assertAlmostEqual(d["inverseMasses"][2], 10)
        self.assertAlmostEqual(d["inverseMasses"][3], 20)
        prev = 0
        for end in d["fabric"]["sets"]:
            indices = d["fabric"]["indices"][prev * 2:end * 2]
            self.assertEqual(len(indices), len(set(indices)))
            prev = end
        self.assertEqual(prev, len(d["fabric"]["restValues"]))

    def test_fractional_pin_and_bad_inputs(self):
        d = FB.cook(POS, TRIS, [1, 1, 0.5, 0], 0.3)
        self.assertAlmostEqual(d["inverseMasses"][2], 5)
        self.assertEqual(d["vertexWeights"][2], 0.5)
        with self.assertRaises(FB.FabricError):
            FB.cook(POS, TRIS, [0] * 4, 0.3)
        with self.assertRaises(FB.FabricError):
            FB.cook(POS, TRIS, [1, 1, 0, 0], 0)


class Relations(unittest.TestCase):
    def test_grid(self):
        pos, tris, pin = strip(4, 3)
        relations(self, FB.cook(pos, tris, pin, 1.0))

    def test_seamed_cape(self):
        """A 4 x 3 cape cut along its middle column for UVs: the copies weld back by particleIds, the render side keeps them."""
        pos, tris, pin = strip(4, 3)
        w = 5
        seam = [y * w + 2 for y in range(4)]
        copy = {v: len(pos) + i for i, v in enumerate(seam)}
        rpos = pos + [pos[v] for v in seam]
        rtris = [tuple(copy[v] if v in copy and all(pos[u][0] >= 2 for u in t) else v for v in t) for t in tris]
        ids = list(range(len(pos))) + seam
        uvs = [(p[0] / 4, p[1] / 3) for p in pos] + [(0.5 + 0.01, pos[v][1] / 3) for v in seam]
        c = P.Cloth(mesh=0, primitive=0, pin=tuple(pin), particle_ids=tuple(ids), where="cape")
        flat = [i for t in rtris for i in t]
        mesh = P.physical_mesh(c, rpos, flat)
        self.assertEqual(len(mesh.positions), len(pos))
        self.assertEqual(mesh.triangles, tris)
        out = FB.cook(mesh.positions, mesh.triangles, mesh.pin, c.mass_kg)
        res = FB.mesh_resource(mesh, out, rpos, uvs, flat)
        relations(self, out, res)
        self.assertEqual(len(res["renderPositions"]), len(rpos))
        self.assertEqual(res["renderSimulationVertexIndices"][len(pos):], seam)
        # the fabric of the welded cape is the fabric of the uncut one
        self.assertEqual(out, FB.cook(pos, tris, pin, 1.0))

    def test_two_pieces(self):
        a_pos, a_tris, a_pin = strip(2, 2)
        b_pos, b_tris, b_pin = strip(3, 1, x0=5.0)
        off = len(a_pos)
        pos, pin = a_pos + b_pos, a_pin + b_pin
        tris = a_tris + [tuple(i + off for i in t) for t in b_tris]
        out = FB.cook(pos, tris, pin, 2.0)
        relations(self, out)
        anchors = out["fabric"]["anchors"]
        self.assertTrue(all(a < off for a in anchors[:off]) and all(a >= off for a in anchors[off:]), "each piece hangs from its own pins")
        # the loose particles carry their share of the whole mass by area (the pinned ones hold the rest)
        areas = self.areas(pos, tris)
        self.assertAlmostEqual(sum(1 / m for m in out["inverseMasses"] if m), 2.0 * sum(a for a, p in zip(areas, pin) if p < 1) / sum(areas), places=9)

    @staticmethod
    def areas(pos, tris):
        out = [0.0] * len(pos)
        for a, b, c in tris:
            x = [pos[b][k] - pos[a][k] for k in range(3)]
            y = [pos[c][k] - pos[a][k] for k in range(3)]
            ar = abs(x[0] * y[1] - x[1] * y[0]) / 2
            for v in (a, b, c):
                out[v] += ar / 3
        return out

    def test_a_bending_pair_that_is_an_edge_stays_structural(self):
        # three triangles round particle 0 (an open cone): across edge 0-2 the opposite particles 1 and 3 share an edge already
        pos = [(0, 1, 0), (1, 0, 0), (0, 0, 1), (-1, 0, -0.5)]
        out = FB.cook(pos, [(0, 1, 2), (0, 2, 3), (0, 3, 1)], [1, 0, 0, 0], 1.0)
        f = out["fabric"]
        pairs = [tuple(f["indices"][i:i + 2]) for i in range(0, len(f["indices"]), 2)]
        self.assertEqual(sorted(pairs), [(0, 1), (0, 2), (0, 3), (1, 2), (1, 3), (2, 3)], "six edges and no bending pair: each opposite pair is an edge")
        self.assertEqual(f["restValues"][pairs.index((1, 3))], ((1 + 1) ** 2 + 0.5 ** 2) ** 0.5)
        self.assertEqual(f["sets"], [2, 4, 6], "three structural batches of two (the six edges of the cone pair up), no bending batch")
        relations(self, out)

    def test_tie_goes_to_the_lower_pin(self):
        # particle 2 is 1 m from pins 0 and 1 alike
        pos = [(-0.5, 0, 0), (0.5, 0, 0), (0, -(3 ** 0.5) / 2, 0)]
        out = FB.cook(pos, [(0, 2, 1)], [1, 1, 0], 1.0)
        self.assertEqual(out["fabric"]["anchors"], [0, 1, 0])

    def test_phase_bound(self):
        pos, tris, pin = strip(2, 2)
        with mock.patch.object(P, "MAX_PHASES", 2), self.assertRaisesRegex(FB.FabricError, "phases"):
            FB.cook(pos, tris, pin, 1.0)


class Serialized(unittest.TestCase):
    def encoded(self):
        c, prim, mesh, out = banner()
        res = FB.mesh_resource(mesh, out, prim["positions"], [(0.0, 0.0)] * 4, prim["indices"])
        return c, mesh, out, Encoder(TYPES).encode("keen::RenderClothMesh", FB.cloth_mesh(c, res))

    def test_round_trip_and_the_mirror(self):
        c, mesh, out, data = self.encoded()
        d = Decoder(TYPES).decode("keen::RenderClothMesh", data)
        q = d["data"]
        self.assertEqual(q["fabric"]["indices"], out["fabric"]["indices"])
        self.assertEqual(q["fabric"]["restValues"], [f32(v) for v in out["fabric"]["restValues"]])
        self.assertEqual(q["simulationInvMasses"], [0.0, 0.0, 10.0, 20.0])
        self.assertEqual(q["skinnedVertexIndices"], [0, 1])
        self.assertEqual(q["renderSimulationVertexIndices"], [0, 1, 2, 3])
        # glTF (x, y, z) is the game's (-x, y, z), once: the banner's right edge is at x = -1
        self.assertEqual([(p["x"], p["y"], p["z"]) for p in q["simulationPositions"]], [(-0.0, 1.0, 0.0), (-1.0, 1.0, 0.0), (-0.0, 0.0, 0.0), (-1.0, 0.0, 0.0)])
        self.assertEqual(q["renderPositions"], q["simulationPositions"])
        self.assertEqual([(t["x"], t["y"], t["z"]) for t in q["renderTriangles"]], TRIS)
        self.assertEqual(q["simulationSkinningData"], [])
        self.assertEqual(d["gravity"], f32(9.81))
        self.assertEqual(d["damping"], f32(0.4))
        self.assertEqual(d["phasesStiffness"], f32(0.8))
        for k, v in FB.FIXED_TUNING.items():
            self.assertEqual(d[k], f32(v), k)
        self.assertEqual(Encoder(TYPES).encode("keen::RenderClothMesh", d), data)

    def test_same_input_same_bytes_in_two_interpreters(self):
        code = ("import hashlib,sys;sys.path[:0]=[sys.argv[1]];import test_fabric as T;print(hashlib.sha256(T.Serialized().encoded()[3]).hexdigest())")
        digests = set()
        for seed in ("1", "2"):
            env = dict(os.environ, PYTHONHASHSEED=seed)
            digests.add(subprocess.run([sys.executable, "-c", code, str(HERE)], env=env, capture_output=True, text=True, check=True).stdout.strip())
        digests.add(__import__("hashlib").sha256(self.encoded()[3]).hexdigest())
        self.assertEqual(len(digests), 1, digests)

    def test_a_value_float32_cannot_hold(self):
        with self.assertRaisesRegex(FB.FabricError, "32-bit float"):
            FB._f32(1e39, "x")
        c, prim, mesh, out = banner()
        far = [(p[0] * 1e39, p[1], p[2]) for p in prim["positions"]]
        with self.assertRaisesRegex(FB.FabricError, r"renderPositions\[1\]"):
            FB.mesh_resource(mesh, out, far, [(0.0, 0.0)] * 4, prim["indices"])
        bad = json.loads(json.dumps(out))
        bad["inverseMasses"][2] = 4e38
        with self.assertRaisesRegex(FB.FabricError, r"inverseMasses\[2\]"):
            FB.check_float32(bad)

    def test_simulation_skin_checks(self):
        c, prim, mesh, out = banner()
        res = FB.mesh_resource(mesh, out, prim["positions"], [(0.0, 0.0)] * 4, prim["indices"], skin=[[(3, 255)], [(3, 200), (4, 55)], [(0, 255)], [(255, 255)]])
        self.assertEqual(res["simulationSkinningData"][1], {"jointIndices": [3, 4, 0, 0], "jointWeights": [200, 55, 0, 0]})
        with self.assertRaisesRegex(FB.FabricError, "8-bit"):
            FB.mesh_resource(mesh, out, prim["positions"], [(0.0, 0.0)] * 4, prim["indices"], skin=[[(256, 255)]] * 4)
        with self.assertRaisesRegex(FB.FabricError, "summing to 255"):
            FB.mesh_resource(mesh, out, prim["positions"], [(0.0, 0.0)] * 4, prim["indices"], skin=[[(1, 254)]] * 4)


if __name__ == "__main__":
    unittest.main()
