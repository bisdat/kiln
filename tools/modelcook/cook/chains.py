# SPDX-License-Identifier: MIT
"""Bone chains (the game's chains2) in a cooked RenderModel (docs/re/models.md section 9: the containers, frame and packing;
docs/kiln/modding/models.md#physics: what an author asks for).

A chain is 2 to 8 joints of the model's own skeleton, from a root whose parent stays animated (the anchor) down one branch to a tip. The game
simulates them on a player's machine; the cook writes what its setup reads:

- the palette (skinningMap): the joints that carry weight and are not simulated, with every anchor (weighted or not), sorted by skeleton
  index, then one reserved slot per simulated joint, in chain and root-to-tip order, holding 0 in skinningMap. Vertices on a simulated joint
  are skinned to its reserved slot. Slot 0 is never a simulated joint's.
- ChainData: per chain its anchor's skeleton index and bind pose (A); per joint its palette slot, its bind relative to the joint above it
  (M0 = inverse(A) B0, Mi = inverse(B(i-1)) Bi) and its inverse bind; one chain collider per chain, empty, chain i naming collider i (all 52
  of the game's chain models have one collider per chain and name it so; 22 of their colliders are empty); the legacy lists empty.
- Chain2: the fixed tuning of 5.3, the asked-for mass, damping, stiffness, bend and radius, and the rest shape: the bind positions in the
  chain's canonical frame (y down the first link), packed into signed 10-bit codes against a millimetre extent.

Matrices are rigid, float32, column vectors, in the game's space (x mirrored from glTF, once: cook/skin.py's targets already are).
"""
import json
import math
import struct

from . import physics as P

DIVISOR = 511000.0  # a packed component is code * extentRestPosition / 511000 (client 0x11e2480 and 0x11e44a0 alike)
MAX_PALETTE = 256  # the render skin's indices and ChainData.joints are u8
RIGID_TOLERANCE = 1e-4  # a bind matrix's rotation part is orthonormal to this, its determinant +1
MAX_REACH = 32.7675  # metres: ceil(2000 L) has to fit the u16 extentMaxAllowedDist
# Fixed v1 tuning (docs/kiln/modding/models.md#physics, from the game's own chains); stiffness, bend, damping, mass and radius come from the request.
CONFIG = {"stretchLimit": 1.0, "friction": 0.1, "linearInertiaHorizontal": round(0.2 * 65535 / 2), "linearInertiaVertical": round(0.2 * 65535 / 2),
          "angularInertia": 0.2, "centrifugalInertia": 0.2, "dragCoefficient": 0.0, "restCurvatureScale": 0.0, "compressionLimit": 0.7,
          "motionRadius": [1.0, 2.0, 3.0, 3.0, 3.0, 3.0, 3.0], "bendStiffnessTipScale": 255, "gravityScale": 100, "angleLimitCos": [0] * 7}
GRAVITY = 9.81
EMPTY_COLLIDER = {"hasSkinning": False, "spheres": [], "capsules": []}


class ChainError(Exception):
    pass


# ---- small math (4x4 rows, column vectors) -----------------------------------------------------------------------

def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def f32m(m):
    """A bind matrix as the game stores it: its 3x4 part in float32."""
    return [[f32(m[r][c]) for c in range(4)] for r in range(3)] + [[0.0, 0.0, 0.0, 1.0]]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def inv_rigid(m):
    """The inverse of a rigid transform: R^T and -R^T t."""
    r = [[m[c][r_] for c in range(3)] for r_ in range(3)]
    t = [-sum(r[i][k] * m[k][3] for k in range(3)) for i in range(3)]
    return [r[0] + [t[0]], r[1] + [t[1]], r[2] + [t[2]], [0.0, 0.0, 0.0, 1.0]]


def pos(m):
    return (m[0][3], m[1][3], m[2][3])


def _sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def rigid_problem(m):
    """Why m is not a rigid unit-scale transform, or None."""
    cols = [tuple(m[r][c] for r in range(3)) for c in range(3)]
    for i in range(3):
        if abs(math.sqrt(_dot(cols[i], cols[i])) - 1) > RIGID_TOLERANCE:
            return f"its axis {'xyz'[i]} has length {math.sqrt(_dot(cols[i], cols[i])):.5f} (scaled)"
        for j in range(i + 1, 3):
            if abs(_dot(cols[i], cols[j])) > RIGID_TOLERANCE:
                return f"its axes {'xyz'[i]} and {'xyz'[j]} are not perpendicular (sheared)"
    det = _dot(cols[0], _cross(cols[1], cols[2]))
    if abs(det - 1) > RIGID_TOLERANCE * 3:
        return f"its determinant is {det:.5f} (mirrored or scaled)"
    return None


def shortest_arc(d):
    """The unit quaternion (x, y, z, w) that turns (0, -1, 0) onto the unit vector d, as client 0xe5370 builds it."""
    a = (0.0, -1.0, 0.0)
    w = 1.0 + _dot(a, d)
    if w < 1e-6:  # d points up: a half turn about -z, the client's perpendicular axis for this a
        return (0.0, 0.0, -1.0, 0.0)
    x, y, z = _cross(a, d)
    n = math.sqrt(x * x + y * y + z * z + w * w)
    return (x / n, y / n, z / n, w / n)


def quat_matrix(q):
    """Rotation matrix (rows) of a unit quaternion."""
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def canonical(anchor, local):
    """The rest positions of a chain in its canonical frame, from the anchor's bind pose A and the joints' stored local matrices M (float32, as
    the game rebuilds them): B0 = A M0, Bi = B(i-1) Mi; d = the first link's direction; Q = shortest arc from (0, -1, 0) to d;
    r_i = R(Q)^T (Bi.pos - B0.pos). r_0 is 0. -> [r_1, ..., r_(n-1)] in double precision."""
    b = []
    cur = anchor
    for m in local:
        cur = mul(cur, m)
        b.append(cur)
    p0 = pos(b[0])
    d = _sub(pos(b[1]), p0)
    ln = math.sqrt(_dot(d, d))
    if ln <= 0:
        raise ChainError("the first link has no length")
    rt = quat_matrix(shortest_arc(tuple(x / ln for x in d)))
    out = []
    for bi in b[1:]:
        v = _sub(pos(bi), p0)
        out.append(tuple(sum(rt[k][i] * v[k] for k in range(3)) for i in range(3)))  # R^T v
    return out


def round_away(x):
    """Round half away from zero."""
    return math.floor(x + 0.5) if x >= 0 else math.ceil(x - 0.5)


def code(x, extent_mm):
    """One signed 10-bit component: round-half-away(x * 511000 / extent), saturated to [-512, 511]."""
    return max(-512, min(511, round_away(x * DIVISOR / extent_mm)))


def pack(r, extent_mm):
    """A rest position as the game's u32: x, y, z codes at bits 0, 10, 20 (two's complement, 10 bits each)."""
    cx, cy, cz = (code(c, extent_mm) for c in r)
    return (cx & 0x3FF) | ((cy & 0x3FF) << 10) | ((cz & 0x3FF) << 20)


def unpack(word, extent_mm):
    """What the client reads back: each 10-bit field sign-extended, times extent / 511000."""
    out = []
    for b in (0, 10, 20):
        c = (word >> b) & 0x3FF
        out.append((c - 1024 if c >= 512 else c) * extent_mm / DIVISOR)
    return tuple(out)


def codes(word):
    return tuple(((word >> b) & 0x3FF) - (1024 if (word >> b) & 0x200 else 0) for b in (0, 10, 20))


# ---- the plan of a model's chains -------------------------------------------------------------------------------------

class Plan:
    """A model's chains against its skeleton: per chain (the request, its joints root to tip, its anchor), and the simulated joints."""

    def __init__(self, chains):
        self.chains = chains  # [(physics.Chain, [joint], anchor)]
        self.simulated = [j for _, js, _ in chains for j in js]
        self.anchors = sorted({a for _, _, a in chains})


def plan(physics, target, where):
    """The chains of `physics` (physics.ModelPhysics) on `target` (skin.Target with parents): each resolved root to tip and checked."""
    parents = getattr(target, "parents", None)
    if parents is None:
        raise ChainError(f"{where}.chains: skeleton {target.guid} is the game's: its own animations move every joint of it, so no joint of it can "
                         "swing. Chains need a skeleton of the mod's own (a skeleton step, a body's model)")
    children = {}
    for j, p in enumerate(parents):
        if p is not None:
            children.setdefault(p, []).append(j)
    out, owner = [], {}
    for ci, c in enumerate(physics.chains):
        cw = c.where or f"{where}.chains[{ci}]"
        root, tip = target.index.get(c.root_joint), target.index.get(c.tip_joint)
        if root is None:
            raise ChainError(f"{cw}.rootJoint: {c.root_joint!r} is not a joint of skeleton {target.guid} ({target.source}, {len(target)} joints)")
        if tip is None:
            raise ChainError(f"{cw}.tipJoint: {c.tip_joint!r} is not a joint of skeleton {target.guid} ({target.source}, {len(target)} joints)")
        path, j = [tip], tip
        while j != root:
            j = parents[j]
            if j is None:
                raise ChainError(f"{cw}.tipJoint: {c.tip_joint!r} is not below {c.root_joint!r} in the skeleton: a chain runs from its root down one branch")
            path.append(j)
            if len(path) > P.CHAIN_JOINTS[1]:
                break
        path.reverse()
        if len(path) > P.CHAIN_JOINTS[1]:
            n = len(path)
            while j != root and j is not None:
                j = parents[j]
                n += 1
            raise ChainError(f"{cw}: {c.root_joint!r} to {c.tip_joint!r} is {n if j == root else 'more than ' + str(P.CHAIN_JOINTS[1])} joints: a chain has at most "
                             f"{P.CHAIN_JOINTS[1]} (the game's setup copies them into fixed 8-particle storage without a check: more would overrun it)")
        anchor = parents[root]
        if anchor is None:
            raise ChainError(f"{cw}.rootJoint: {c.root_joint!r} is the skeleton's root: a chain hangs from a joint that stays animated, its root's parent")
        for jj in path:
            if jj in owner:
                raise ChainError(f"{cw}: joint {target.names[jj]!r} already swings in {owner[jj]}: a joint is in one chain")
            owner[jj] = cw
        out.append((c, path, anchor))
    for c, path, anchor in out:
        if anchor in owner:
            raise ChainError(f"{c.where}: its anchor {target.names[anchor]!r} swings in {owner[anchor]}: a chain hangs from a joint that stays animated")
        if anchor >= 256:
            raise ChainError(f"{c.where}: its anchor {target.names[anchor]!r} is joint {anchor}: the game keeps an anchor in 8 bits, below 256")
    for c, path, anchor in out:
        for k, jj in enumerate(path):
            below = [x for x in children.get(jj, []) if k + 1 >= len(path) or x != path[k + 1]]
            if below:
                raise ChainError(f"{c.where}: joint {target.names[jj]!r} of the chain has {target.names[below[0]]!r} below it outside the chain: a chain does not "
                                 "branch, and nothing hangs from its joints (it would follow the animation, not the swing)")
        for jj in [anchor] + path:
            bad = rigid_problem(target.bind[jj])
            if bad:
                raise ChainError(f"{c.where}: joint {target.names[jj]!r}'s bind pose is not rigid: {bad}. Apply scale to the armature (unit-scale bones in this version)")
    return Plan(out)


def palette(lods, plan):
    """skinningMap and the slot of each skeleton joint for a model with chains: (smap, model_index). The joints that carry weight and are not
    simulated, with every anchor, sorted; then one reserved slot per simulated joint (skinningMap 0 there)."""
    weighted = {j for lod in lods for p in lod for inf in p["influences"] for j, _ in inf}
    ordinary = sorted((weighted - set(plan.simulated)) | set(plan.anchors))
    smap = ordinary + [0] * len(plan.simulated)
    if len(smap) > MAX_PALETTE:
        raise ChainError(f"{len(ordinary)} joints carry weight or anchor a chain, and {len(plan.simulated)} swing: a model addresses {MAX_PALETTE} (u8)")
    index = {j: i for i, j in enumerate(ordinary)}
    for k, j in enumerate(plan.simulated):
        index[j] = len(ordinary) + k
    return smap, index


def _matrix43(m):
    """A 4x4 (rows, column vectors) as a keen::Matrix43: the columns x, y, z, then the translation."""
    return {"rot": {c: {a: float(m[ai][ci]) for ai, a in enumerate("xyz")} for ci, c in enumerate("xyz")},
            "pos": {a: float(m[ai][3]) for ai, a in enumerate("xyz")}}


def chain_guid(mod, model, root, tip):
    """A chain's GUID: the mod's namespace, model-chain: and the canonical JSON of [model path, root joint, tip joint] (names as the glTF has them)."""
    from anicook.graphgen import derive
    return derive(mod, "model-chain", json.dumps([model, root, tip], ensure_ascii=False, separators=(",", ":")))


def chain_data(plan, target, model_index, mod, model):
    """keen::RenderModelChainData of a model's chains. -> (dict, report)."""
    if not mod:
        raise ChainError("a chain's GUID follows from the mod's id, which this cook was not given")
    out = {"chains": [], "chains2": [], "configs": [], "chainColliders": [dict(EMPTY_COLLIDER) for _ in plan.chains], "anchorHierarchyJoints": [], "anchorDefaultPose": [],
           "anchorRestPosition": [], "joints": [], "modelMatrices": [], "skinToJointMatrix": [], "restPosition": [], "constraintJoints": [], "constraints": []}
    report = []
    for c, path, anchor in plan.chains:
        a = f32m(target.bind[anchor])
        binds = [f32m(target.bind[j]) for j in path]
        local = [f32m(mul(inv_rigid(a), binds[0]))] + [f32m(mul(inv_rigid(binds[i - 1]), binds[i])) for i in range(1, len(path))]
        links = [math.dist(pos(binds[i]), pos(binds[i - 1])) for i in range(1, len(path))]
        for i, ln in enumerate(links):
            if not math.isfinite(ln) or ln <= 1e-6:
                raise ChainError(f"{c.where}: joints {target.names[path[i]]!r} and {target.names[path[i + 1]]!r} are at one place: every link has a length")
        reach = sum(links)
        if reach > MAX_REACH:
            raise ChainError(f"{c.where}: the chain is {reach:.3f} m long: at most {MAX_REACH} m (its safety radius is kept in millimetres, 16 bits)")
        rest = canonical(a, local)
        extent_m = max(abs(x) for r in rest for x in r)
        extent = math.ceil(1000 * extent_m)
        if not 1 <= extent <= 65535:
            raise ChainError(f"{c.where}: the chain's rest shape spans {extent_m:.4f} m: its extent has to be 1 to 65535 mm")
        max_dist = math.ceil(2000 * reach)
        radius = round_away(1000 * c.particle_radius)
        begin = len(out["joints"])
        cfg = dict(CONFIG, stiffness=float(c.stiffness), bendStiffness=float(c.bend_stiffness), inverseMass=1.0 / c.particle_mass_kg, damping=float(c.damping),
                   restPosition=[pack(r, extent) for r in rest], extentRestPosition=extent, extentMaxAllowedDist=max_dist, particleRadius=radius,
                   count=len(path))
        guid = chain_guid(mod, model, c.root_joint, c.tip_joint)
        out["chains2"].append({"propagateRotation": True, "gravity": GRAVITY, "config": cfg, "guid": guid, "colliderIndex": len(out["chains2"]),
                               "jointBeginIndex": begin, "jointEndIndex": begin + len(path)})
        out["anchorHierarchyJoints"].append(anchor)
        out["anchorDefaultPose"].append(_matrix43(a))
        for j, m, b in zip(path, local, binds):
            out["joints"].append(model_index[j])
            out["modelMatrices"].append(_matrix43(m))
            out["skinToJointMatrix"].append(_matrix43(f32m(inv_rigid(b))))
        report.append({"root": c.root_joint, "tip": c.tip_joint, "joints": len(path), "anchor": target.names[anchor], "extentMm": extent,
                       "maxDistMm": max_dist, "guid": guid, "slots": [model_index[j] for j in path]})
    return out, report


# ---- reading a cooked chain back (validate.py, tests) -------------------------------------------------------------

def u8_list(v):
    """A decoded BlobArray<uint8>: tools/kfc gives one longer than 64 as {"bytes": hex}."""
    return list(bytes.fromhex(v["bytes"])) if isinstance(v, dict) else list(v)


def matrix_of(m43):
    """A decoded keen::Matrix43 as 4x4 rows."""
    rot, p = m43["rot"], m43["pos"]
    return [[rot[c][a] for c in "xyz"] + [p[a]] for a in "xyz"] + [[0.0, 0.0, 0.0, 1.0]]


def decoded_rest(chain_data_value, i):
    """Chain i's rest positions as the client unpacks them, and as its stored matrices give them in the canonical frame: (unpacked, expected)."""
    c = chain_data_value["chains2"][i]
    cfg = c["config"]
    local = [matrix_of(m) for m in chain_data_value["modelMatrices"][c["jointBeginIndex"]:c["jointEndIndex"]]]
    expected = canonical(matrix_of(chain_data_value["anchorDefaultPose"][i]), local)
    return [unpack(w, cfg["extentRestPosition"]) for w in cfg["restPosition"]], expected
