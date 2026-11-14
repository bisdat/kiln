# SPDX-License-Identifier: MIT
"""Physics format 1: the cloth and bone chains a mod asks the cook to write into its models (docs/kiln/modding/models.md#physics).

One input, one tree. A mod writes `assets/physics.json`, or gives a body block `cloth` and `chains` (Kiln.Blocks'
ModelClothBlock and ModelChainBlock, the same fields in blocks/*.json): both become this module's tree before the cook
reads a single vertex, and a model that both name is refused rather than one of them chosen.

    { "format": 1,
      "models": {
        "models/banner.gltf": {
          "cloth":  [ { "mesh": 0, "primitive": 0, "particleIds": [0, 1, 2, 3], "pin": [1, 1, 0, 0], "massKg": 0.3,
                        "colliders": [ { "type": "sphere", "x": 0, "y": 0.5, "z": 0.2, "radius": 0.1 } ] } ],
          "chains": [ { "rootJoint": "pendant_0", "tipJoint": "pendant_2" } ] } } }

- A model's key is its glTF file's path in the mod (forward slashes, no "." or ".." parts).
- A cloth entry picks a glTF mesh and one of its primitives by index, as the file numbers them.
- `particleIds`: one integer per POSITION element of that primitive. Equal ids weld the copies a UV or normal seam made
  (they must sit within 1e-6 m of each other and carry the same skin), and the first time an id appears gives its
  particle's place: particles are numbered densely in that order. Without it every vertex is its own particle.
- `pin`: one value in [0, 1] per particle (in that dense order). 1 holds the particle on the model (it moves with its
  joint, or stays put on an unskinned model); 0 lets it hang; values between make it heavier and limit its motion.
  Every connected piece of cloth needs a particle with pin 1.
- `massKg`: the whole cloth's mass, spread over the particles by the area around each.
- `colliders`: spheres and capsules the cloth does not pass through, in the model's bind pose, metres, glTF axes; on
  one named joint (`joint`) or rigid. The colliders of all cloth entries of a model are the model's one list: at most
  32 spheres, a capsule counting as two.
- A chain runs from `rootJoint` (the first joint that swings: its parent stays animated) to `tipJoint` along the
  skeleton, 2 to 8 joints, no branches.

Everything else of the game's cloth and chain tuning is fixed in v1 (chapter 16e, 5.3). What this format cannot say
(sleeves, simulated morph targets, per-vertex stiffness, a second cloth LOD) is refused where it would be asked for.
Each refusal starts with the field path, so an author finds the value: `assets/physics.json: models["x.gltf"].cloth[0].pin[3]: ...`.
"""
import json
import math
from dataclasses import dataclass, field
from pathlib import PurePosixPath

FORMAT = 1
FILE = "assets/physics.json"

# Ranges (chapter 16e, 5.3: the v1 tuning surface). Closed intervals unless the name says open.
MASS_KG = (0.001, 100.0)
GRAVITY = (0.0, 50.0)
CLOTH_DAMPING = (0.0, 0.99)
CLOTH_STIFFNESS_OPEN_LOW = (0.0, 1.0)  # (0, 1]
COLLIDER_RADIUS_OPEN_LOW = (0.0, 8.0)  # (0, 8] metres
CHAIN_DAMPING = (0.0, 0.99)
CHAIN_STIFFNESS = (1.0, 1_000_000.0)
CHAIN_BEND = (0.0, 1_000_000.0)
CHAIN_RADIUS = (0.0, 1.0)

CLOTH_DEFAULTS = {"massKg": 1.0, "gravity": 9.81, "damping": 0.4, "stiffness": 0.8}
CHAIN_DEFAULTS = {"particleMassKg": 1.0, "damping": 0.01, "stiffness": 10000.0, "bendStiffness": 5.0, "particleRadius": 0.0}

# Hard writer bounds: field widths of the cooked containers (chapter 16e, 2.1 and 5.3), not what the game's own models reach.
MAX_CLOTH_MESHES = 255  # RenderModelMesh.clothMeshIndex is a u8, 0 meaning "not cloth"
MAX_PARTICLES = 65535  # skinnedVertexIndices and renderSimulationVertexIndices are u16
MAX_RENDER_VERTICES = 65535
MAX_PHASES = 65535  # the runtime's phase indices are u16
MAX_COLLIDER_SPHERES = 32  # spheres + 2 x capsules, after the client expands capsules (client 0xe7360)
MAX_ID = 2**31 - 1  # Kiln.Blocks' ModelClothBlock.ParticleIds are ints
CHAIN_JOINTS = (2, 8)  # both setup paths build fixed 8-particle storage; more overruns it (client 0xe7e30)
WELD_M = 1e-6  # welded seam copies agree in bind position within this
MIN_AREA = 1e-12  # m^2: a triangle at or below it is degenerate (the fabric's rule)

CLOTH = "modelcook.cloth"
CHAINS = "modelcook.chains2"


class PhysicsError(Exception):
    """Bad physics input. The message starts with the field path."""


@dataclass(frozen=True)
class Sphere:
    x: float
    y: float
    z: float
    radius: float
    joint: str | None = None

    def to_json(self):
        return {"type": "sphere", "x": self.x, "y": self.y, "z": self.z, "radius": self.radius, "joint": self.joint}


@dataclass(frozen=True)
class Capsule:
    a: Sphere
    b: Sphere

    @property
    def joint(self):
        return self.a.joint

    def to_json(self):
        return {"type": "capsule", "a": _sphere_fields(self.a), "b": _sphere_fields(self.b)}


def _sphere_fields(s):
    return {"x": s.x, "y": s.y, "z": s.z, "radius": s.radius, "joint": s.joint}


@dataclass(frozen=True)
class Cloth:
    mesh: int
    primitive: int
    pin: tuple
    particle_ids: tuple | None = None
    mass_kg: float = CLOTH_DEFAULTS["massKg"]
    gravity: float = CLOTH_DEFAULTS["gravity"]
    damping: float = CLOTH_DEFAULTS["damping"]
    stiffness: float = CLOTH_DEFAULTS["stiffness"]
    colliders: tuple = ()
    where: str = field(default="", compare=False)

    def to_json(self):
        return {"mesh": self.mesh, "primitive": self.primitive, "particleIds": list(self.particle_ids) if self.particle_ids is not None else None,
                "pin": list(self.pin), "massKg": self.mass_kg, "gravity": self.gravity, "damping": self.damping, "stiffness": self.stiffness,
                "colliders": [c.to_json() for c in self.colliders]}


@dataclass(frozen=True)
class Chain:
    root_joint: str
    tip_joint: str
    particle_mass_kg: float = CHAIN_DEFAULTS["particleMassKg"]
    damping: float = CHAIN_DEFAULTS["damping"]
    stiffness: float = CHAIN_DEFAULTS["stiffness"]
    bend_stiffness: float = CHAIN_DEFAULTS["bendStiffness"]
    particle_radius: float = CHAIN_DEFAULTS["particleRadius"]
    where: str = field(default="", compare=False)

    def to_json(self):
        return {"rootJoint": self.root_joint, "tipJoint": self.tip_joint, "particleMassKg": self.particle_mass_kg, "damping": self.damping,
                "stiffness": self.stiffness, "bendStiffness": self.bend_stiffness, "particleRadius": self.particle_radius}


@dataclass(frozen=True)
class ModelPhysics:
    """What one model asks for. `source` names where it was declared (a file, or a block)."""
    model: str
    cloth: tuple = ()
    chains: tuple = ()
    source: str = field(default=FILE, compare=False)
    where: str = field(default="", compare=False)

    def features(self):
        """The cook features this model needs (cook/features.py)."""
        return ([CLOTH] if self.cloth else []) + ([CHAINS] if self.chains else [])

    def colliders(self):
        """The model's one collider list: every cloth entry's, in order, each once."""
        out = []
        for c in self.cloth:
            for x in c.colliders:
                if x not in out:
                    out.append(x)
        return tuple(out)

    def to_json(self):
        return {"cloth": [c.to_json() for c in self.cloth], "chains": [c.to_json() for c in self.chains]}


@dataclass
class Physics:
    """The tree: {model path: ModelPhysics}."""
    models: dict = field(default_factory=dict)

    def get(self, model):
        return self.models.get(model)

    def to_json(self):
        return {"format": FORMAT, "models": {k: self.models[k].to_json() for k in sorted(self.models)}}


# ------------------------------------------------------------------------------------------------- reading ----

def _at(where, key):
    return f"{where}[{key}]" if isinstance(key, int) else f"{where}.{key}" if where else key


def _refuse(message):
    raise PhysicsError(message)


def _strict_pairs(where):
    def hook(pairs):
        out = {}
        for k, v in pairs:
            if k in out:
                raise PhysicsError(f"{where}: the key {k!r} appears twice in one object")
            out[k] = v
        return out
    return hook


def loads(text, source=FILE):
    """Physics from the text of a physics.json. Duplicate keys and NaN/Infinity are refused, as the C# reader does."""
    try:
        doc = json.loads(text, object_pairs_hook=_strict_pairs(source), parse_constant=lambda c: _refuse(f"{source}: {c} is not a JSON number"))
    except ValueError as e:
        if isinstance(e, PhysicsError):
            raise
        raise PhysicsError(f"{source}: not JSON ({e})") from e
    return parse(doc, source)


def load(path, source=FILE):
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except UnicodeDecodeError as e:
        raise PhysicsError(f"{source}: not UTF-8 text ({e})") from e
    return loads(text, source)


def _keys(v, where, allowed, required=()):
    if not isinstance(v, dict):
        raise PhysicsError(f"{where}: expected an object")
    bad = [k for k in v if k not in allowed]
    if bad:
        raise PhysicsError(f"{_at(where, bad[0])}: not a field here (known: {', '.join(allowed)})")
    for k in required:
        if k not in v:
            raise PhysicsError(f"{_at(where, k)}: missing")


def _int(v, where, lo=0, hi=MAX_ID):
    if type(v) is not int:  # bool is an int to Python, not to the format
        raise PhysicsError(f"{where}: {v!r} is not an integer")
    if not lo <= v <= hi:
        raise PhysicsError(f"{where}: {v} is outside [{lo}, {hi}]")
    return v


def _num(v, where, rng, open_low=False):
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise PhysicsError(f"{where}: {v!r} is not a number")
    v = float(v)
    lo, hi = rng
    if not math.isfinite(v) or (v <= lo if open_low else v < lo) or v > hi:
        raise PhysicsError(f"{where}: {v!r} is outside {'(' if open_low else '['}{lo:g}, {hi:g}]")
    return v


def _coord(v, where):
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(float(v)):
        raise PhysicsError(f"{where}: {v!r} is not a finite number")
    return float(v)


def _name(v, where, optional=False):
    if v is None and optional:
        return None
    if not isinstance(v, str) or not v:
        raise PhysicsError(f"{where}: {v!r} is not a joint name" + (" (or null for a rigid collider)" if optional else ""))
    return v


def model_key(key, where):
    """A model's key, checked: the glTF file's path in the mod, as Kiln.Blocks and the cook spell it."""
    if not isinstance(key, str) or not key:
        raise PhysicsError(f"{where}: a model is named by its glTF file's path in the mod")
    p = PurePosixPath(key)
    if ("\\" in key or key.startswith("/") or (len(key) > 1 and key[1] == ":") or any(part in ("", ".", "..") for part in key.split("/"))
            or p.as_posix() != key):
        raise PhysicsError(f"{where}: {key!r} is not a path in the mod (forward slashes, no leading /, no \".\" or \"..\" parts)")
    if p.suffix.lower() not in (".gltf", ".glb"):
        raise PhysicsError(f"{where}: {key!r} is not a glTF file (.gltf or .glb)")
    return key


def parse_sphere(v, where, typed=True):
    allowed = ("type", "x", "y", "z", "radius", "joint") if typed else ("x", "y", "z", "radius", "joint", "type")
    _keys(v, where, allowed, ("x", "y", "z", "radius"))
    if "type" in v and v["type"] != "sphere":
        raise PhysicsError(f"{_at(where, 'type')}: {v['type']!r}: an end of a capsule is a sphere")
    return Sphere(_coord(v["x"], _at(where, "x")), _coord(v["y"], _at(where, "y")), _coord(v["z"], _at(where, "z")),
                  _num(v["radius"], _at(where, "radius"), COLLIDER_RADIUS_OPEN_LOW, open_low=True), _name(v.get("joint"), _at(where, "joint"), optional=True))


def parse_collider(v, where):
    if not isinstance(v, dict):
        raise PhysicsError(f"{where}: expected an object with \"type\": \"sphere\" or \"capsule\"")
    kind = v.get("type")
    if kind == "sphere":
        return parse_sphere(v, where)
    if kind == "capsule":
        _keys(v, where, ("type", "a", "b"), ("a", "b"))
        a, b = parse_sphere(v["a"], _at(where, "a"), typed=False), parse_sphere(v["b"], _at(where, "b"), typed=False)
        if a.joint != b.joint:
            raise PhysicsError(f"{_at(where, 'b')}.joint: {b.joint!r}: both ends of a capsule follow one joint ({a.joint!r}), the format has one skin record per capsule")
        if math.dist((a.x, a.y, a.z), (b.x, b.y, b.z)) <= WELD_M:
            raise PhysicsError(f"{where}: both ends of the capsule are at one point: make it a sphere")
        return Capsule(a, b)
    raise PhysicsError(f"{_at(where, 'type')}: {kind!r}: a collider is \"sphere\" or \"capsule\"")


def expanded_spheres(colliders):
    """How many spheres a collider list is to the client: a capsule is two."""
    return sum(2 if isinstance(c, Capsule) else 1 for c in colliders)


def parse_cloth(v, where):
    _keys(v, where, ("mesh", "primitive", "particleIds", "pin", "massKg", "gravity", "damping", "stiffness", "colliders"), ("mesh", "primitive", "pin"))
    ids = v.get("particleIds")
    if ids is not None:
        if not isinstance(ids, list) or not ids:
            raise PhysicsError(f"{_at(where, 'particleIds')}: one integer per POSITION element of the primitive")
        if len(ids) > MAX_RENDER_VERTICES:
            raise PhysicsError(f"{_at(where, 'particleIds')}: {len(ids)} vertices: a cloth mesh has at most {MAX_RENDER_VERTICES}")
        ids = tuple(_int(x, f"{_at(where, 'particleIds')}[{i}]") for i, x in enumerate(ids))
    pin = v["pin"]
    if not isinstance(pin, list) or not pin:
        raise PhysicsError(f"{_at(where, 'pin')}: one value in [0, 1] per particle")
    pin = tuple(_num(x, f"{_at(where, 'pin')}[{i}]", (0.0, 1.0)) for i, x in enumerate(pin))
    if len(pin) > MAX_PARTICLES:
        raise PhysicsError(f"{_at(where, 'pin')}: {len(pin)} particles: a cloth mesh has at most {MAX_PARTICLES}")
    if ids is not None and len(set(ids)) != len(pin):
        raise PhysicsError(f"{_at(where, 'pin')}: {len(pin)} values for {len(set(ids))} particles (distinct particleIds)")
    cols = v.get("colliders", [])
    if not isinstance(cols, list):
        raise PhysicsError(f"{_at(where, 'colliders')}: a list of spheres and capsules")
    cols = tuple(parse_collider(c, f"{_at(where, 'colliders')}[{i}]") for i, c in enumerate(cols))
    if expanded_spheres(cols) > MAX_COLLIDER_SPHERES:
        raise PhysicsError(f"{_at(where, 'colliders')}: {expanded_spheres(cols)} spheres (a capsule is two): the client takes at most {MAX_COLLIDER_SPHERES}")
    return Cloth(mesh=_int(v["mesh"], _at(where, "mesh")), primitive=_int(v["primitive"], _at(where, "primitive")), pin=pin, particle_ids=ids,
                 mass_kg=_num(v.get("massKg", CLOTH_DEFAULTS["massKg"]), _at(where, "massKg"), MASS_KG),
                 gravity=_num(v.get("gravity", CLOTH_DEFAULTS["gravity"]), _at(where, "gravity"), GRAVITY),
                 damping=_num(v.get("damping", CLOTH_DEFAULTS["damping"]), _at(where, "damping"), CLOTH_DAMPING),
                 stiffness=_num(v.get("stiffness", CLOTH_DEFAULTS["stiffness"]), _at(where, "stiffness"), CLOTH_STIFFNESS_OPEN_LOW, open_low=True),
                 colliders=cols, where=where)


def parse_chain(v, where):
    _keys(v, where, ("rootJoint", "tipJoint", "particleMassKg", "damping", "stiffness", "bendStiffness", "particleRadius"), ("rootJoint", "tipJoint"))
    root, tip = _name(v["rootJoint"], _at(where, "rootJoint")), _name(v["tipJoint"], _at(where, "tipJoint"))
    if root == tip:
        raise PhysicsError(f"{_at(where, 'tipJoint')}: {tip!r} is the root too: a chain has {CHAIN_JOINTS[0]} to {CHAIN_JOINTS[1]} joints")
    return Chain(root, tip,
                 particle_mass_kg=_num(v.get("particleMassKg", CHAIN_DEFAULTS["particleMassKg"]), _at(where, "particleMassKg"), MASS_KG),
                 damping=_num(v.get("damping", CHAIN_DEFAULTS["damping"]), _at(where, "damping"), CHAIN_DAMPING),
                 stiffness=_num(v.get("stiffness", CHAIN_DEFAULTS["stiffness"]), _at(where, "stiffness"), CHAIN_STIFFNESS),
                 bend_stiffness=_num(v.get("bendStiffness", CHAIN_DEFAULTS["bendStiffness"]), _at(where, "bendStiffness"), CHAIN_BEND),
                 particle_radius=_num(v.get("particleRadius", CHAIN_DEFAULTS["particleRadius"]), _at(where, "particleRadius"), CHAIN_RADIUS),
                 where=where)


def parse_model(key, v, source, where=None):
    """One model's entry ({"cloth": [...], "chains": [...]}). `source` names the file or block it came from."""
    where = where if where is not None else f"{source}: models[{json.dumps(key)}]"
    model_key(key, where)
    _keys(v, where, ("cloth", "chains"))
    cloth_v, chains_v = v.get("cloth", []), v.get("chains", [])
    for k, x in (("cloth", cloth_v), ("chains", chains_v)):
        if not isinstance(x, list):
            raise PhysicsError(f"{_at(where, k)}: a list")
    if not cloth_v and not chains_v:
        raise PhysicsError(f"{where}: no cloth and no chain: leave the model out")
    if len(cloth_v) > MAX_CLOTH_MESHES:
        raise PhysicsError(f"{_at(where, 'cloth')}: {len(cloth_v)} cloth meshes: a model has at most {MAX_CLOTH_MESHES}")
    cloth = tuple(parse_cloth(c, f"{_at(where, 'cloth')}[{i}]") for i, c in enumerate(cloth_v))
    seen = {}
    for c in cloth:
        k = (c.mesh, c.primitive)
        if k in seen:
            raise PhysicsError(f"{c.where}: mesh {c.mesh} primitive {c.primitive} is already cloth in {seen[k]}")
        seen[k] = c.where
    chains = tuple(parse_chain(c, f"{_at(where, 'chains')}[{i}]") for i, c in enumerate(chains_v))
    pairs = {}
    for c in chains:
        if (c.root_joint, c.tip_joint) in pairs:
            raise PhysicsError(f"{c.where}: the chain {c.root_joint!r} to {c.tip_joint!r} is already declared in {pairs[(c.root_joint, c.tip_joint)]}")
        pairs[(c.root_joint, c.tip_joint)] = c.where
    mp = ModelPhysics(key, cloth, chains, source, where)
    n = expanded_spheres(mp.colliders())
    if n > MAX_COLLIDER_SPHERES:
        raise PhysicsError(f"{_at(where, 'cloth')}: the model's colliders come to {n} spheres (a capsule is two, each collider counted once): the client takes at most {MAX_COLLIDER_SPHERES}")
    return mp


def parse(doc, source=FILE):
    """Physics from a parsed physics.json."""
    _keys(doc, source, ("format", "models", "comment"), ("format", "models"))
    if doc["format"] != FORMAT or type(doc["format"]) is not int:
        raise PhysicsError(f"{source}: format {doc['format']!r}: this cook reads format {FORMAT}")
    models = doc["models"]
    if not isinstance(models, dict):
        raise PhysicsError(f"{source}: models: an object, glTF path -> {{\"cloth\": [...], \"chains\": [...]}}")
    return merge(*[Physics({k: parse_model(k, v, source)}) for k, v in models.items()])


def merge(*parts):
    """One tree from the file and the blocks. A model declared twice (also in another spelling of its case: one file on
    Windows) is refused with both places named: nothing decides which one wins."""
    out, folded = {}, {}
    for part in parts:
        for key, mp in part.models.items():
            other = folded.get(key.lower())
            if other is not None:
                raise PhysicsError(f"{mp.where}: {key!r} already has physics from {other.where}: declare a model's cloth and chains in one place")
            folded[key.lower()] = mp
            out[key] = mp
    return Physics(out)


# --------------------------------------------------------------------------------------------- the mesh rules ----

@dataclass
class PhysicalMesh:
    """A cloth entry applied to its primitive: the particles and triangles the fabric is cooked from.
    ids[v]: the particle of original vertex v (render -> simulation); positions, pin, influences: per particle;
    triangles: over particles, in the primitive's order."""
    ids: list
    positions: list
    pin: tuple
    triangles: list
    influences: list | None = None


def physical_mesh(cloth, positions, indices, influences=None, where=None):
    """Applies `cloth` to a primitive's bind-pose `positions` (one per POSITION element) and triangle `indices`, with
    its quantized skin `influences` per vertex ([(joint, u8 weight)], or None for an unskinned primitive). Every rule of
    chapter 16e 5.1 that needs the mesh is checked here: ids, welds, pins, faces, isolated particles, pinned pieces."""
    where = where or cloth.where
    nv = len(positions)
    if not 1 <= nv <= MAX_RENDER_VERTICES:
        raise PhysicsError(f"{where}: the primitive has {nv} vertices: a cloth mesh has 1 to {MAX_RENDER_VERTICES}")
    if len(indices) % 3 or not indices:
        raise PhysicsError(f"{where}: the primitive has {len(indices)} indices: a cloth mesh is a list of triangles")
    raw = cloth.particle_ids if cloth.particle_ids is not None else tuple(range(nv))
    if len(raw) != nv:
        raise PhysicsError(f"{_at(where, 'particleIds')}: {len(raw)} ids for the primitive's {nv} POSITION elements: one each")
    dense, ids, first = {}, [], []
    for v, pid in enumerate(raw):
        if pid not in dense:
            dense[pid] = len(first)
            first.append(v)
        ids.append(dense[pid])
    n = len(first)
    if n > MAX_PARTICLES:
        raise PhysicsError(f"{_at(where, 'particleIds')}: {n} particles: a cloth mesh has at most {MAX_PARTICLES}")
    if len(cloth.pin) != n:
        raise PhysicsError(f"{_at(where, 'pin')}: {len(cloth.pin)} values for {n} particles: one each, in the order the particles first appear")
    for v, p in enumerate(ids):
        f = first[p]
        if f == v:
            continue
        d = math.dist(positions[v], positions[f])
        if d > WELD_M:
            raise PhysicsError(f"{_at(where, 'particleIds')}[{v}]: vertex {v} welds to vertex {f} (id {raw[v]}) but lies {d:.3g} m from it: welded copies sit within {WELD_M:g} m")
        if influences is not None and sorted(influences[v]) != sorted(influences[f]):
            raise PhysicsError(f"{_at(where, 'particleIds')}[{v}]: vertex {v} welds to vertex {f} (id {raw[v]}) but has another skin ({influences[v]} against {influences[f]})")
    pos = [tuple(float(c) for c in positions[f]) for f in first]
    tris, seen, edges = [], {}, {}
    for t in range(0, len(indices), 3):
        tw = f"{where}: triangle {t // 3} (indices {indices[t]}, {indices[t + 1]}, {indices[t + 2]})"
        if any(not 0 <= i < nv for i in indices[t:t + 3]):
            raise PhysicsError(f"{tw}: an index outside the primitive's {nv} vertices")
        a, b, c = (ids[i] for i in indices[t:t + 3])
        if len({a, b, c}) != 3:
            raise PhysicsError(f"{tw}: degenerate: it uses particle {a if a in (b, c) else b} twice (after welding)")
        e1 = [pos[b][k] - pos[a][k] for k in range(3)]
        e2 = [pos[c][k] - pos[a][k] for k in range(3)]
        cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        if math.sqrt(sum(x * x for x in cr)) / 2 <= MIN_AREA:
            raise PhysicsError(f"{tw}: degenerate: it has no area")
        key = tuple(sorted((a, b, c)))
        if key in seen:
            raise PhysicsError(f"{tw}: the same particles as triangle {seen[key]} (a duplicate or back face): cloth is one-sided, one triangle per face")
        seen[key] = t // 3
        for i, j in ((a, b), (b, c), (c, a)):
            edges.setdefault((min(i, j), max(i, j)), []).append(t // 3)
        tris.append((a, b, c))
    for (i, j), ts in sorted(edges.items()):
        if len(ts) > 2:
            raise PhysicsError(f"{where}: particles {i} and {j} share an edge of {len(ts)} triangles ({', '.join(map(str, ts))}): non-manifold, an edge has at most two")
    used = {p for t in tris for p in t}
    if len(used) != n:
        lone = min(set(range(n)) - used)
        raise PhysicsError(f"{where}: particle {lone} (vertex {first[lone]}) is in no triangle: an isolated particle has nothing to hang from")
    parent = list(range(n))

    def root(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for i, j in edges:
        parent[root(i)] = root(j)
    pinned = {root(p) for p in range(n) if cloth.pin[p] == 1.0}
    for p in range(n):
        if root(p) not in pinned:
            raise PhysicsError(f"{_at(where, 'pin')}: the piece of cloth with particle {p} (vertex {first[p]}) has no particle with pin 1: it would fall away")
    infl = [list(influences[f]) for f in first] if influences is not None else None
    return PhysicalMesh(ids, pos, cloth.pin, tris, infl)
