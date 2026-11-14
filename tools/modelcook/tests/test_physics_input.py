# SPDX-License-Identifier: MIT
"""Physics format 1 input (cook/physics.py) and the cook's feature rows (cook/features.py), without game files: the banner sidecar
(tests/fixtures/banner, synthetic: the research's reference banner) parses, each refusal names its field, the mesh rules of
docs/kiln/modding/models.md#physics hold on small hand-made primitives, and the feature rows that wait for a game test are the client gate's entries too.

    python3 -m unittest discover -s tools/modelcook/tests -p test_physics_input.py
"""
import copy
import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from cook import features as F, physics as P  # noqa: E402

BANNER = HERE / "fixtures" / "banner"
GATES = HERE.parent.parent.parent / "native" / "core" / "gates"

FULL = {"format": 1, "models": {"models/cape.glb": {
    "cloth": [{"mesh": 0, "primitive": 1, "particleIds": [7, 3, 3, 9], "pin": [1, 0, 0], "massKg": 2, "gravity": 9.81, "damping": 0.4, "stiffness": 0.8,
               "colliders": [{"type": "sphere", "x": 0, "y": 1, "z": 0, "radius": 0.2, "joint": "spine"},
                             {"type": "capsule", "a": {"x": 0, "y": 0, "z": 0, "radius": 0.1, "joint": None}, "b": {"x": 0, "y": 1, "z": 0, "radius": 0.1}}]}],
    "chains": [{"rootJoint": "tail_0", "tipJoint": "tail_3", "particleMassKg": 1, "damping": 0.01, "stiffness": 10000, "bendStiffness": 5, "particleRadius": 0.02}]}}}
CAPE = 'assets/physics.json: models["models/cape.glb"]'


def full(edits):
    """FULL with edits: {path tuple: value}; a value of ... deletes the key."""
    d = copy.deepcopy(FULL)
    for path, v in edits.items():
        o = d
        for k in path[:-1]:
            o = o[k]
        if v is ...:
            del o[path[-1]]
        else:
            o[path[-1]] = v
    return d


def cloth_path(*rest):
    return ("models", "models/cape.glb", "cloth", 0) + rest


class Sidecar(unittest.TestCase):
    def test_banner_parses(self):
        ph = P.load(BANNER / "physics.json")
        mp = ph.get("banner.gltf")
        self.assertEqual(len(mp.cloth), 1)
        c = mp.cloth[0]
        self.assertEqual((c.mesh, c.primitive, c.particle_ids, c.pin, c.mass_kg), (0, 0, (0, 1, 2, 3), (1.0, 1.0, 0.0, 0.0), 0.3))
        self.assertEqual((c.gravity, c.damping, c.stiffness, c.colliders), (9.81, 0.4, 0.8, ()))
        self.assertEqual(mp.chains, ())
        self.assertEqual(mp.features(), [P.CLOTH])

    def test_full_form_and_its_canonical_tree(self):
        ph = P.parse(FULL)
        mp = ph.get("models/cape.glb")
        self.assertEqual(mp.features(), [P.CLOTH, P.CHAINS])
        caps = mp.cloth[0].colliders
        self.assertIsInstance(caps[0], P.Sphere)
        self.assertIsInstance(caps[1], P.Capsule)
        self.assertEqual(caps[0].joint, "spine")
        self.assertIsNone(caps[1].joint)
        self.assertEqual(mp.chains[0].particle_radius, 0.02)
        # the tree says every field, defaults written out: the same request in the short form gives the same tree
        short = {"format": 1, "models": {"models/cape.glb": {"cloth": [{"mesh": 0, "primitive": 1, "particleIds": [7, 3, 3, 9], "pin": [1, 0, 0], "massKg": 2,
                                                                       "colliders": FULL["models"]["models/cape.glb"]["cloth"][0]["colliders"]}],
                                                             "chains": [{"rootJoint": "tail_0", "tipJoint": "tail_3", "particleRadius": 0.02}]}}}
        self.assertEqual(json.dumps(P.parse(short).to_json(), sort_keys=True), json.dumps(ph.to_json(), sort_keys=True))
        self.assertEqual(P.parse(short), ph)
        j = ph.to_json()["models"]["models/cape.glb"]["cloth"][0]
        self.assertEqual(j["colliders"][1], {"type": "capsule", "a": {"x": 0.0, "y": 0.0, "z": 0.0, "radius": 0.1, "joint": None},
                                             "b": {"x": 0.0, "y": 1.0, "z": 0.0, "radius": 0.1, "joint": None}})

    def test_defaults(self):
        mp = P.parse({"format": 1, "models": {"a.gltf": {"cloth": [{"mesh": 0, "primitive": 0, "pin": [1]}], "chains": [{"rootJoint": "r", "tipJoint": "t"}]}}}).get("a.gltf")
        c, ch = mp.cloth[0], mp.chains[0]
        self.assertEqual((c.particle_ids, c.mass_kg, c.gravity, c.damping, c.stiffness), (None, 1.0, 9.81, 0.4, 0.8))
        self.assertEqual((ch.particle_mass_kg, ch.damping, ch.stiffness, ch.bend_stiffness, ch.particle_radius), (1.0, 0.01, 10000.0, 5.0, 0.0))

    def refused(self, doc, field, text=""):
        with self.assertRaises(P.PhysicsError) as e:
            P.parse(doc) if isinstance(doc, dict) else P.loads(doc)
        msg = str(e.exception)
        self.assertTrue(msg.startswith(field), f"{msg!r} does not start with {field!r}")
        self.assertIn(text, msg)
        return msg

    def test_each_refusal_names_its_field(self):
        c = CAPE + ".cloth[0]"
        cases = [
            (full({("format",): 2}), "assets/physics.json: format 2", "format 1"),
            (full({("models",): []}), "assets/physics.json: models", "an object"),
            (full({("extra",): 1}), "assets/physics.json.extra", "not a field here"),
            (full({cloth_path("pins"): [1]}), c + ".pins", "not a field here"),
            (full({cloth_path("pin"): ...}), c + ".pin", "missing"),
            (full({cloth_path("mesh"): -1}), c + ".mesh", "outside"),
            (full({cloth_path("mesh"): 0.5}), c + ".mesh", "not an integer"),
            (full({cloth_path("primitive"): True}), c + ".primitive", "not an integer"),
            (full({cloth_path("particleIds"): [0, -1, 1, 2]}), c + ".particleIds[1]", "outside"),
            (full({cloth_path("particleIds"): [0, 1.0, 1, 2]}), c + ".particleIds[1]", "not an integer"),
            (full({cloth_path("particleIds"): []}), c + ".particleIds", "one integer per POSITION"),
            (full({cloth_path("pin"): [1, 1.5, 0]}), c + ".pin[1]", "outside [0, 1]"),
            (full({cloth_path("pin"): [1, -0.1, 0]}), c + ".pin[1]", "outside [0, 1]"),
            (full({cloth_path("pin"): [1, "0", 0]}), c + ".pin[1]", "not a number"),
            (full({cloth_path("pin"): [1, 0]}), c + ".pin", "2 values for 3 particles"),
            (full({cloth_path("massKg"): 0}), c + ".massKg", "outside [0.001, 100]"),
            (full({cloth_path("massKg"): 100.5}), c + ".massKg", "outside [0.001, 100]"),
            (full({cloth_path("gravity"): 51}), c + ".gravity", "outside [0, 50]"),
            (full({cloth_path("damping"): 1}), c + ".damping", "outside [0, 0.99]"),
            (full({cloth_path("stiffness"): 0}), c + ".stiffness", "outside (0, 1]"),
            (full({cloth_path("stiffness"): 1.01}), c + ".stiffness", "outside (0, 1]"),
            (full({cloth_path("colliders"): {}}), c + ".colliders", "a list"),
            (full({cloth_path("colliders", 0, "type"): "box"}), c + ".colliders[0].type", "\"sphere\" or \"capsule\""),
            (full({cloth_path("colliders", 0, "radius"): 0}), c + ".colliders[0].radius", "outside (0, 8]"),
            (full({cloth_path("colliders", 0, "radius"): 8.5}), c + ".colliders[0].radius", "outside (0, 8]"),
            (full({cloth_path("colliders", 0, "x"): "1"}), c + ".colliders[0].x", "not a finite number"),
            (full({cloth_path("colliders", 0, "joint"): ""}), c + ".colliders[0].joint", "not a joint name"),
            (full({cloth_path("colliders", 0, "z"): ...}), c + ".colliders[0].z", "missing"),
            (full({cloth_path("colliders", 1, "b", "joint"): "spine"}), c + ".colliders[1].b.joint", "one joint"),
            (full({cloth_path("colliders", 1, "b", "y"): 0}), c + ".colliders[1]", "one point"),
            (full({cloth_path("colliders", 1, "a", "type"): "sphere"}), c + ".colliders[1].a.type", "not a field here"),
            (full({("models", "models/cape.glb", "chains", 0, "rootJoint"): ""}), CAPE + ".chains[0].rootJoint", "not a joint name"),
            (full({("models", "models/cape.glb", "chains", 0, "tipJoint"): "tail_0"}), CAPE + ".chains[0].tipJoint", "the root too"),
            (full({("models", "models/cape.glb", "chains", 0, "particleMassKg"): 0.0001}), CAPE + ".chains[0].particleMassKg", "outside"),
            (full({("models", "models/cape.glb", "chains", 0, "damping"): 0.995}), CAPE + ".chains[0].damping", "outside"),
            (full({("models", "models/cape.glb", "chains", 0, "stiffness"): 0.5}), CAPE + ".chains[0].stiffness", "outside [1, 1e+06]"),
            (full({("models", "models/cape.glb", "chains", 0, "bendStiffness"): 2e6}), CAPE + ".chains[0].bendStiffness", "outside"),
            (full({("models", "models/cape.glb", "chains", 0, "particleRadius"): 1.5}), CAPE + ".chains[0].particleRadius", "outside [0, 1]"),
            (full({("models", "models/cape.glb", "chains", 0, "length"): 3}), CAPE + ".chains[0].length", "not a field here"),
            (full({("models", "models/cape.glb", "sleeves"): []}), CAPE + ".sleeves", "not a field here"),
            ({"format": 1, "models": {"x.glb": {"cloth": [], "chains": []}}}, 'assets/physics.json: models["x.glb"]', "no cloth and no chain"),
        ]
        for doc, field, text in cases:
            with self.subTest(field=field):
                self.refused(doc, field, text)

    def test_model_keys_are_paths_in_the_mod(self):
        entry = FULL["models"]["models/cape.glb"]
        for key in ("../cape.glb", "/models/cape.glb", "models\\cape.glb", "./cape.glb", "models//cape.glb", "C:/cape.glb", "models/cape.fbx", "models/./cape.glb", ""):
            with self.subTest(key=key):
                self.refused({"format": 1, "models": {key: entry}}, f"assets/physics.json: models[{json.dumps(key)}]")
        P.parse({"format": 1, "models": {"Models/Cape.GLB": entry}})

    def test_strict_json(self):
        self.refused('{"format": 1, "format": 1, "models": {}}', "assets/physics.json", "appears twice")
        self.refused('{"format": 1, "models": {"a.gltf": {"cloth": [{"mesh": 0, "primitive": 0, "pin": [NaN]}]}}}', "assets/physics.json", "NaN is not a JSON number")
        self.refused('{"format": 1, "models": {"a.gltf": {"cloth": [{"mesh": 0, "primitive": 0, "pin": [Infinity]}]}}}', "assets/physics.json", "Infinity")
        self.refused('{"format": 1, "models": ', "assets/physics.json", "not JSON")

    def test_one_primitive_once_one_chain_once(self):
        c = FULL["models"]["models/cape.glb"]["cloth"][0]
        msg = self.refused(full({("models", "models/cape.glb", "cloth"): [c, dict(c)]}), CAPE + ".cloth[1]", "already cloth")
        self.assertIn(CAPE + ".cloth[0]", msg)
        ch = FULL["models"]["models/cape.glb"]["chains"][0]
        self.refused(full({("models", "models/cape.glb", "chains"): [ch, dict(ch, damping=0.2)]}), CAPE + ".chains[1]", "already declared")

    def test_cloth_meshes_per_model(self):
        many = [{"mesh": i, "primitive": 0, "pin": [1]} for i in range(P.MAX_CLOTH_MESHES)]
        self.assertEqual(len(P.parse({"format": 1, "models": {"a.gltf": {"cloth": many}}}).get("a.gltf").cloth), 255)
        many.append({"mesh": 255, "primitive": 0, "pin": [1]})
        self.refused({"format": 1, "models": {"a.gltf": {"cloth": many}}}, 'assets/physics.json: models["a.gltf"].cloth', "at most 255")

    def test_collider_budget_is_the_models(self):
        def sphere(i):
            return {"type": "sphere", "x": i, "y": 0, "z": 0, "radius": 0.1}

        def capsule(i):
            return {"type": "capsule", "a": {"x": i, "y": 0, "z": 0, "radius": 0.1}, "b": {"x": i, "y": 1, "z": 0, "radius": 0.1}}
        c = {"mesh": 0, "primitive": 0, "pin": [1]}
        # 32 spheres fit, a capsule counting two
        P.parse({"format": 1, "models": {"a.gltf": {"cloth": [dict(c, colliders=[sphere(i) for i in range(30)] + [capsule(99)])]}}})
        self.refused({"format": 1, "models": {"a.gltf": {"cloth": [dict(c, colliders=[sphere(i) for i in range(31)] + [capsule(99)])]}}},
                     'assets/physics.json: models["a.gltf"].cloth[0].colliders', "33 spheres")
        # two cloth entries with the same 20 colliders: the model's list holds each once
        two = [dict(c, colliders=[sphere(i) for i in range(20)]), dict(c, mesh=1, colliders=[sphere(i) for i in range(20)])]
        self.assertEqual(len(P.parse({"format": 1, "models": {"a.gltf": {"cloth": two}}}).get("a.gltf").colliders()), 20)
        two[1]["colliders"] = [sphere(i) for i in range(20, 33)]
        self.refused({"format": 1, "models": {"a.gltf": {"cloth": two}}}, 'assets/physics.json: models["a.gltf"].cloth', "come to 33 spheres")

    def test_a_model_has_physics_from_one_place(self):
        entry = FULL["models"]["models/cape.glb"]
        a = P.parse(FULL)
        b = P.Physics({"models/cape.glb": P.parse_model("models/cape.glb", entry, "blocks/npc.json", "blocks/npc.json: bodies.golem")})
        with self.assertRaises(P.PhysicsError) as e:
            P.merge(a, b)
        self.assertTrue(str(e.exception).startswith("blocks/npc.json: bodies.golem"))
        self.assertIn(CAPE, str(e.exception))
        c = P.Physics({"MODELS/cape.glb": P.parse_model("MODELS/cape.glb", entry, "blocks/npc.json", "blocks/npc.json: bodies.golem")})
        with self.assertRaisesRegex(P.PhysicsError, "already has physics"):
            P.merge(a, c)
        with self.assertRaisesRegex(P.PhysicsError, "already has physics"):
            P.loads(json.dumps({"format": 1, "models": {"a.gltf": entry, "A.gltf": entry}}))
        other = P.Physics({"models/other.glb": P.parse_model("models/other.glb", entry, "blocks/npc.json")})
        self.assertEqual(sorted(P.merge(a, other).models), ["models/cape.glb", "models/other.glb"])


class SpecHash(unittest.TestCase):
    """The spec form Kiln.Blocks checks a body against (Npc/NpcPhysics.cs SpecBytes): managed/tests/Kiln.Blocks.Tests/ModelPhysicsTests.cs holds the
    same two hashes for the same requests, typed and as blocks/*.json."""
    ENTRY = {"cloth": [{"mesh": 0, "primitive": 1, "particleIds": [7, 3, 3, 9], "pin": [1, 0, 0.25], "massKg": 0.3,
                        "colliders": [{"type": "sphere", "x": 0, "y": 1.5, "z": -0.1, "radius": 0.15, "joint": "spine"},
                                      {"type": "capsule", "a": {"x": 0.1, "y": 1, "z": 0, "radius": 0.1, "joint": "j2"},
                                       "b": {"x": 0.1, "y": 0.4, "z": 0, "radius": 0.08, "joint": "j2"}}]}],
             "chains": [{"rootJoint": "z\u00f6pf_0", "tipJoint": "z\u00f6pf_2", "particleRadius": 0.02}]}
    FULL = "e5c0cae78d515c24b5e5ec5301716a18b3de6f0278709821363d6c6c2b11b5f1"
    MINIMAL = "48447f8115000ff8fb08bf190e914f9ebd9e8367f9ddea662c3aa179cea5b2f4"

    def test_the_hashes_kiln_blocks_writes_too(self):
        self.assertEqual(P.spec_hash(P.parse_model("models/cape.gltf", self.ENTRY, "blocks/npc.json")), self.FULL)
        self.assertEqual(P.spec_hash(P.parse_model("m.gltf", {"cloth": [{"mesh": 0, "primitive": 0, "pin": [1]}]}, "x")), self.MINIMAL)

    def test_the_form(self):
        b = P.spec_bytes(P.parse_model("m.gltf", {"cloth": [{"mesh": 0, "primitive": 0, "pin": [1]}]}, "x"))
        self.assertEqual(b, b"kiln-physics-1;l2[l1[l9[i0;i0;n;l1[d3ff0000000000000;]d3ff0000000000000;d40239eb851eb851f;d3fd999999999999a;d3fe999999999999a;l0[]]]l0[]]")
        # the model's path is not in it, every value is
        e = json.loads(json.dumps(self.ENTRY))
        self.assertEqual(P.spec_hash(P.parse_model("other.gltf", e, "x")), self.FULL)
        e["chains"][0]["particleRadius"] = 0.03
        self.assertNotEqual(P.spec_hash(P.parse_model("other.gltf", e, "x")), self.FULL)
        # defaults written out: the short form is the long one
        long = {"cloth": [{"mesh": 0, "primitive": 0, "pin": [1], "massKg": 1, "gravity": 9.81, "damping": 0.4, "stiffness": 0.8, "colliders": []}]}
        self.assertEqual(P.spec_hash(P.parse_model("m.gltf", long, "x")), self.MINIMAL)


# a 2 x 1 strip of quads in the x/y plane, top row pinned: vertices 0..5, 0-1-2 on top
STRIP = [(0, 1, 0), (1, 1, 0), (2, 1, 0), (0, 0, 0), (1, 0, 0), (2, 0, 0)]
STRIP_TRIS = [0, 3, 1, 1, 3, 4, 1, 4, 2, 2, 4, 5]


def cloth(pin, ids=None, **kw):
    return P.Cloth(mesh=0, primitive=0, pin=tuple(float(p) for p in pin), particle_ids=tuple(ids) if ids is not None else None, where="c", **kw)


class MeshRules(unittest.TestCase):
    def refused(self, c, positions, indices, text, influences=None):
        with self.assertRaises(P.PhysicsError) as e:
            P.physical_mesh(c, positions, indices, influences)
        self.assertTrue(str(e.exception).startswith("c"), str(e.exception))
        self.assertIn(text, str(e.exception))

    def test_plain_strip(self):
        m = P.physical_mesh(cloth([1, 1, 1, 0, 0, 0]), STRIP, STRIP_TRIS)
        self.assertEqual(m.ids, list(range(6)))
        self.assertEqual(m.triangles, [(0, 3, 1), (1, 3, 4), (1, 4, 2), (2, 4, 5)])
        self.assertIsNone(m.influences)

    def test_seam_copies_weld_by_id(self):
        # the middle column twice (a UV seam): 6 and 7 are copies of 1 and 4, used by the right half
        pos = STRIP + [(1, 1, 0.0000005), (1, 0, 0)]
        tris = [0, 3, 1, 1, 3, 4, 6, 7, 2, 2, 7, 5]
        ids = [10, 11, 12, 13, 14, 15, 11, 14]
        m = P.physical_mesh(cloth([1, 1, 1, 0, 0, 0], ids), pos, tris)
        self.assertEqual(m.ids, [0, 1, 2, 3, 4, 5, 1, 4])
        self.assertEqual(m.triangles, [(0, 3, 1), (1, 3, 4), (1, 4, 2), (2, 4, 5)])
        self.assertEqual(m.positions[1], (1.0, 1.0, 0.0))  # the first copy's position
        # dense ids follow first appearance, whatever the numbers
        m2 = P.physical_mesh(cloth([0, 1, 1, 1, 0, 0], [5, 0, 1, 2, 3, 4]), [STRIP[3], STRIP[0], STRIP[1], STRIP[2], STRIP[4], STRIP[5]], [1, 0, 2, 2, 0, 4, 2, 4, 3, 3, 4, 5])
        self.assertEqual(m2.ids, [0, 1, 2, 3, 4, 5])

    def test_weld_rules(self):
        pos = STRIP + [(1, 1, 0.000002), (1, 0, 0)]
        tris = [0, 3, 1, 1, 3, 4, 6, 7, 2, 2, 7, 5]
        ids = [0, 1, 2, 3, 4, 5, 1, 4]
        self.refused(cloth([1, 1, 1, 0, 0, 0], ids), pos, tris, "c.particleIds[6]: vertex 6 welds to vertex 1")
        pos[6] = (1, 1, 0)
        skin = [[(0, 255)]] * 8
        skin = [list(s) for s in skin]
        skin[7] = [(1, 255)]
        self.refused(cloth([1, 1, 1, 0, 0, 0], ids), pos, tris, "c.particleIds[7]: vertex 7 welds to vertex 4 (id 4) but has another skin", influences=skin)
        skin[7] = [(0, 255)]
        P.physical_mesh(cloth([1, 1, 1, 0, 0, 0], ids), pos, tris, skin)

    def test_counts(self):
        self.refused(cloth([1, 1, 1, 0, 0, 0], [0, 1, 2, 3, 4]), STRIP, STRIP_TRIS, "c.particleIds: 5 ids for the primitive's 6 POSITION elements")
        self.refused(cloth([1, 1, 1, 0, 0]), STRIP, STRIP_TRIS, "c.pin: 5 values for 6 particles")
        self.refused(cloth([1] * 6), STRIP, STRIP_TRIS[:-1], "a list of triangles")
        self.refused(cloth([1] * 6), STRIP, STRIP_TRIS[:-1] + [6], "an index outside")

    def test_vertex_bound(self):
        n = P.MAX_RENDER_VERTICES + 1
        self.refused(cloth([1] * 3), [(0, 0, 0)] * n, [0, 1, 2], f"{n} vertices")

    def test_faces(self):
        pin = [1, 1, 1, 0, 0, 0]
        self.refused(cloth(pin), STRIP, STRIP_TRIS + [0, 0, 1], "triangle 4 (indices 0, 0, 1): degenerate: it uses particle 0 twice")
        # degenerate after welding: 3 and 4 made one particle
        self.refused(cloth([1, 1, 1, 0, 0], [0, 1, 2, 3, 3, 4]), [STRIP[0], STRIP[1], STRIP[2], STRIP[3], STRIP[3], STRIP[5]], STRIP_TRIS, "degenerate")
        self.refused(cloth(pin), STRIP[:2] + [(2, 1, 0)] + STRIP[3:], [0, 1, 2] + STRIP_TRIS, "no area")
        self.refused(cloth(pin), STRIP, STRIP_TRIS + [3, 1, 0], "triangle 4 (indices 3, 1, 0): the same particles as triangle 0")
        self.refused(cloth(pin), STRIP, STRIP_TRIS + [0, 1, 3], "a duplicate or back face")
        # a third triangle on edge 1-4 (to a vertex off the strip)
        self.refused(cloth(pin + [0]), STRIP + [(1, 0.5, 1)], STRIP_TRIS + [1, 4, 6], "particles 1 and 4 share an edge of 3 triangles")

    def test_isolated_particle(self):
        self.refused(cloth([1, 1, 1, 0, 0, 0, 0]), STRIP + [(5, 5, 5)], STRIP_TRIS, "particle 6 (vertex 6) is in no triangle")

    def test_every_piece_is_pinned(self):
        # two strips side by side, only the first pinned
        far = [(x + 10, y, z) for x, y, z in STRIP]
        tris = STRIP_TRIS + [i + 6 for i in STRIP_TRIS]
        self.refused(cloth([1, 1, 1, 0, 0, 0] + [0.99] * 6), STRIP + far, tris, "c.pin: the piece of cloth with particle 6 (vertex 6) has no particle with pin 1")
        m = P.physical_mesh(cloth([1, 1, 1, 0, 0, 0] + [0, 0, 1, 0, 0, 0]), STRIP + far, tris)
        self.assertEqual(len(m.positions), 12)
        self.refused(cloth([0.5] * 6), STRIP, STRIP_TRIS, "no particle with pin 1")


class FeatureRows(unittest.TestCase):
    def test_the_rows_and_their_reason(self):
        self.assertEqual([f.name for f in F.ROWS], ["modelcook.cloth", "modelcook.chains2"])
        for f in F.ROWS:
            self.assertFalse(f.default)
            self.assertEqual(f.reason, "game test pending: 16e-cc16e")
            self.assertFalse(F.is_on(f.name))
            self.assertTrue(F.is_on(f.name, {f.name}))
        self.assertFalse(F.is_on("modelcook.cloth", {"modelcook.chains2"}))
        with self.assertRaises(KeyError):
            F.is_on("modelcook.sleeves")

    def test_the_recipe_key(self):
        self.assertEqual(F.experimental({"format": 1}), frozenset())
        self.assertEqual(F.experimental({"experimental": ["modelcook.cloth"]}), {"modelcook.cloth"})
        with self.assertRaisesRegex(F.FeatureError, r"assets/cook.json: experimental\[1\]: 'modelcook.clth' is not a cook feature"):
            F.experimental({"experimental": ["modelcook.cloth", "modelcook.clth"]})
        with self.assertRaisesRegex(F.FeatureError, "is a list of cook feature names"):
            F.experimental({"experimental": "modelcook.cloth"})

    def test_requested_but_off_is_refused_by_name(self):
        where = 'assets/physics.json: models["banner.gltf"].cloth'
        with self.assertRaises(F.FeatureOff) as e:
            F.require("modelcook.cloth", frozenset(), where)
        msg = str(e.exception)
        self.assertTrue(msg.startswith(where))
        for part in ("modelcook.cloth", "game test pending: 16e-cc16e", "\"experimental\": [\"modelcook.cloth\"]"):
            self.assertIn(part, msg)
        F.require("modelcook.cloth", {"modelcook.cloth"}, where)
        with self.assertRaises(F.FeatureOff):
            F.require("modelcook.chains2", {"modelcook.cloth"}, where)

    def test_one_warning_line(self):
        self.assertIsNone(F.warning(frozenset()))
        w = F.warning({"modelcook.cloth", "modelcook.chains2"})
        self.assertNotIn("\n", w)
        self.assertIn("modelcook.cloth (game test pending: 16e-cc16e)", w)
        self.assertIn("modelcook.chains2 (game test pending: 16e-cc16e)", w)


@unittest.skipUnless(GATES.is_dir(), "the client gate's list is not here (a packed SDK): it is checked in the repository")
class GateListAgrees(unittest.TestCase):
    """Every cook row that waits for a game test is held by the client gate with the same checks, and every gate entry named
    modelcook.* is such a row (ruling e-01: one gate, two places that say so). The cook never reads the gate list."""

    def gates(self):
        out = {}
        for f in sorted(GATES.glob("*.json")):
            for g in json.loads(f.read_text())["gates"]:
                out[g["name"]] = (g["checks"], g.get("side"), f.name)
        return out

    def test_rows_waiting_for_a_game_test_are_gate_entries(self):
        gates = self.gates()
        for f in F.ROWS:
            if f.reason and f.reason.startswith(F.PENDING):
                with self.subTest(row=f.name):
                    self.assertIn(f.name, gates, f"{f.name} is off ({f.reason}) but no native/core/gates/*.json holds it")
                    self.assertEqual(gates[f.name][0], list(f.checks))
                    self.assertIsNone(gates[f.name][1], "a cook row is held on both sides")

    def test_modelcook_gate_entries_are_rows_waiting_for_a_game_test(self):
        for name, (checks, _, file) in self.gates().items():
            if name.startswith("modelcook."):
                with self.subTest(gate=name):
                    row = F.BY_NAME.get(name)
                    self.assertIsNotNone(row, f"{file} holds {name}, which is not a row of cook/features.py")
                    self.assertEqual(row.reason, F.PENDING + ", ".join(checks))


if __name__ == "__main__":
    unittest.main()
