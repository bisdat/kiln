# SPDX-License-Identifier: MIT
"""Kiln's cloth fabric, format 1: a deterministic distance-constraint fabric for the game's cloth solver (chapter 16e, 5.2).

Kiln's own algorithm, not the game's cooker: no NVIDIA cooker and no GPL code. Its reference is the research probe
fabric_v1.py; tests/test_fabric.py holds its banner output as the golden. The output maps one to one onto the reflected
keen::RenderClothFabricResource arrays:

1. Constraints. Every unique triangle edge (lower particle, higher particle) is a structural constraint. Every edge with
   exactly two triangles adds the pair of their opposite particles as a bending constraint, unless that pair is already
   structural. Its rest value is the bind-pose distance in metres; a zero distance is refused.
2. Batches. Structural constraints first, then bending, each sorted by particle pair. Within a family each constraint goes
   into the lowest-numbered batch that touches neither of its particles, else a new batch. Flattened in family and batch
   order: indices [i, j, ...], restValues, sets (cumulative constraint count at each batch's end), phaseIndices 0..n-1.
   stiffnessValues stays empty: one stiffness for every phase (RenderClothMesh.phasesStiffness).
3. Tethers. Multi-source shortest paths over the structural edges (edge cost: its length), from every particle with pin 1.
   A tie goes to the lower pin particle. One anchor and length per particle; a pinned particle is its own anchor at 0.
4. Masses. A particle's area is a third of each of its triangles' areas; m_i = massKg * A_i / sum(A),
   inverseMass_i = (1 - pin_i) / m_i, vertexWeight_i = 1 - pin_i.

Distances and areas are worked out in double precision from the bind positions; each float is rounded to float32 once,
when the model is encoded, and a value float32 cannot hold is refused here.
"""
import heapq
import math
import struct

from . import physics as P

# Every other cloth scalar of the game's RenderClothMesh, fixed in v1 (chapter 16e, 5.3: the values of a fully decoded
# vanilla axe, the same on the game's cloth meshes generally). Gravity, damping and phase stiffness come from the request.
FIXED_TUNING = {
    "windStrength": 2.0, "dragCoefficient": 0.6, "liftCoefficient": 0.3, "linearInertia": 0.6, "angularInertia": 0.3,
    "centrifugalInertia": 0.3, "tetherStiffness": 0.6, "phasesCompressionLimit": 0.8, "phasesStretchLimit": 1.0,
    "motionConstraintsStiffness": 1.0, "collisionFriction": 0.5, "solverFrequency": 300.0,
    "motionConstraintsUnlimitedRadius": 4.0, "motionConstraintsMaxRadius": 4.0,
}
FLT_MAX = 3.4028234663852886e38


class FabricError(Exception):
    pass


def cook(positions, triangles, pin, mass_kg):
    """positions: bind positions per particle (metres); triangles: (a, b, c) over particles; pin per particle; mass_kg: the
    whole cloth's. -> {"fabric": {numParticles, phaseIndices, sets, restValues, stiffnessValues, indices, anchors,
    tetherLengths, triangles}, "inverseMasses", "vertexWeights"} as Python numbers (double precision)."""
    n = len(positions)
    if not 1 <= n <= P.MAX_PARTICLES or any(len(v) != 3 or not all(math.isfinite(x) for x in v) for v in positions):
        raise FabricError(f"{n} particles, or a position that is not three finite numbers")
    if len(pin) != n or not P.MASS_KG[0] <= mass_kg <= P.MASS_KG[1] or any(not 0 <= p <= 1 for p in pin):
        raise FabricError("pin or mass out of range")
    areas = [0.0] * n
    edges, pairs, seen = {}, {}, set()

    def distance(i, j):
        return math.dist(positions[i], positions[j])

    for tri in triangles:
        a, b, c = tri
        if len(set(tri)) != 3 or any(v < 0 or v >= n for v in tri):
            raise FabricError(f"triangle {tri}: degenerate or out of range")
        key = tuple(sorted(tri))
        if key in seen:
            raise FabricError(f"triangle {tri}: a duplicate or back face")
        seen.add(key)
        x = [positions[b][k] - positions[a][k] for k in range(3)]
        y = [positions[c][k] - positions[a][k] for k in range(3)]
        cr = [x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]]
        area = math.sqrt(sum(z * z for z in cr)) / 2
        if area <= P.MIN_AREA:
            raise FabricError(f"triangle {tri}: no area")
        for v in tri:
            areas[v] += area / 3
        for i, j, o in ((a, b, c), (b, c, a), (c, a, b)):
            e = (min(i, j), max(i, j))
            edges.setdefault(e, []).append(o)
            pairs[e] = 0
    for e, opp in sorted(edges.items()):
        if len(opp) > 2:
            raise FabricError(f"edge {e}: non-manifold")
        if len(opp) == 2 and opp[0] != opp[1]:
            pairs.setdefault((min(opp), max(opp)), 1)
    if any(v == 0 for v in areas):
        raise FabricError("a particle in no triangle")
    # Family then particle-pair order; greedy first fit, no particle twice in one batch.
    batches = []
    for family in (0, 1):
        sets, used = [], []
        for pair, f in sorted(pairs.items()):
            if f != family:
                continue
            index = next((i for i, u in enumerate(used) if not (set(pair) & u)), len(sets))
            if index == len(sets):
                sets.append([])
                used.append(set())
            sets[index].append(pair)
            used[index].update(pair)
        batches.extend(sets)
    if len(batches) > P.MAX_PHASES:
        raise FabricError(f"{len(batches)} phases: the runtime's phase indices are 16-bit, at most {P.MAX_PHASES}")
    indices, rest, ends = [], [], []
    for batch in batches:
        for i, j in batch:
            d = distance(i, j)
            if d <= 0:
                raise FabricError(f"particles {i} and {j}: a constraint of zero length")
            indices.extend((i, j))
            rest.append(d)
        ends.append(len(rest))
    # Multi-source shortest paths on the structural edges; an exact tie goes to the lower pin particle.
    adj = [[] for _ in range(n)]
    for i, j in edges:
        adj[i].append((j, distance(i, j)))
        adj[j].append((i, distance(i, j)))
    best = [(math.inf, n)] * n
    q = []
    for i, p in enumerate(pin):
        if p == 1:
            best[i] = (0.0, i)
            heapq.heappush(q, (0.0, i, i))
    while q:
        dist, anchor, i = heapq.heappop(q)
        if best[i] != (dist, anchor):
            continue
        for j, length in adj[i]:
            candidate = (dist + length, anchor)
            if candidate < best[j]:
                best[j] = candidate
                heapq.heappush(q, (*candidate, j))
    if any(not math.isfinite(v[0]) for v in best):
        raise FabricError("a connected piece of cloth has no particle with pin 1")
    total = sum(areas)
    inverse = [(1 - pin[i]) / (mass_kg * areas[i] / total) for i in range(n)]
    fabric = {"numParticles": n, "phaseIndices": list(range(len(batches))), "sets": ends, "restValues": rest, "stiffnessValues": [],
              "indices": indices, "anchors": [a for _, a in best], "tetherLengths": [d for d, _ in best], "triangles": [i for t in triangles for i in t]}
    out = {"fabric": fabric, "inverseMasses": inverse, "vertexWeights": [1 - p for p in pin]}
    check_float32(out)
    return out


def _f32(v, what):
    if not math.isfinite(v) or abs(v) > FLT_MAX:
        raise FabricError(f"{what}: {v!r} does not fit a 32-bit float")
    try:
        struct.pack("<f", v)
    except OverflowError as e:
        raise FabricError(f"{what}: {v!r} does not fit a 32-bit float") from e


def check_float32(result):
    """Refuse a result with a value float32 cannot hold (an inverse mass of a sliver particle, say)."""
    f = result["fabric"]
    for name, vs in (("restValues", f["restValues"]), ("tetherLengths", f["tetherLengths"]), ("inverseMasses", result["inverseMasses"]),
                     ("vertexWeights", result["vertexWeights"])):
        for i, v in enumerate(vs):
            _f32(v, f"{name}[{i}]")


def _v3(p):
    return {"x": float(p[0]), "y": float(p[1]), "z": float(p[2])}


def _u3(t):
    return {"x": t[0], "y": t[1], "z": t[2]}


def to_game(p):
    """A glTF-space point in the game's space: x mirrored, as cook/geometry.to_game does it, once."""
    return (-p[0], p[1], p[2])


def mesh_resource(mesh, result, render_positions, render_uvs, render_indices, skin=None):
    """keen::RenderClothMeshResource of one cloth mesh. mesh: physics.PhysicalMesh (glTF space, bind pose); result: cook()'s;
    render_positions (glTF space), render_uvs, render_indices: the primitive's own vertices and triangles, the ones its
    RenderModelMesh draws. skin: per particle [(hierarchy joint, u8 weight)] or None (unskinned). Positions come out in the
    game's space; triangles keep their order, as the model's index buffer does."""
    nr = len(render_positions)
    if not 1 <= nr <= P.MAX_RENDER_VERTICES or len(mesh.ids) != nr:
        raise FabricError(f"{nr} render vertices for {len(mesh.ids)} particle ids")
    if len(render_uvs) != nr:
        raise FabricError(f"{len(render_uvs)} UVs for {nr} render vertices")
    if len(render_indices) % 3 or any(not 0 <= i < nr for i in render_indices):
        raise FabricError("render triangles out of range")
    inv = result["inverseMasses"]
    sim_skin = []
    if skin is not None:
        if len(skin) != len(mesh.positions):
            raise FabricError(f"{len(skin)} skin records for {len(mesh.positions)} particles")
        for i, inf in enumerate(skin):
            if not inf or len(inf) > 4 or sum(w for _, w in inf) != 255:
                raise FabricError(f"particle {i}: a skin of 1 to 4 joints with weights summing to 255")
            if any(not 0 <= j < 256 for j, _ in inf):
                raise FabricError(f"particle {i}: joint {max(j for j, _ in inf)}: simulation skin joints are 8-bit, below 256")
            js = [j for j, _ in inf] + [0] * (4 - len(inf))
            ws = [w for _, w in inf] + [0] * (4 - len(inf))
            sim_skin.append({"jointIndices": js, "jointWeights": ws})
    for name, vs in (("renderPositions", [c for p in render_positions for c in p]), ("simulationPositions", [c for p in mesh.positions for c in p]),
                     ("renderUvs", [c for uv in render_uvs for c in uv])):
        for i, v in enumerate(vs):
            _f32(v, f"{name}[{i // (2 if name == 'renderUvs' else 3)}]")
    return {
        "fabric": result["fabric"],
        "simulationPositions": [_v3(to_game(p)) for p in mesh.positions],
        "simulationInvMasses": list(inv),
        "simulationStiffnesses": [],
        "simulationVertexWeights": list(result["vertexWeights"]),
        "simulationTriangles": [_u3(t) for t in mesh.triangles],
        "simulationBlendshapeDeltas": [],
        "simulationSkinningData": sim_skin,
        "skinnedVertexIndices": [i for i, m in enumerate(inv) if m == 0],
        "renderPositions": [_v3(to_game(p)) for p in render_positions],
        "renderUvs": [{"x": float(uv[0]), "y": float(uv[1])} for uv in render_uvs],
        "renderTriangles": [_u3(tuple(render_indices[t:t + 3])) for t in range(0, len(render_indices), 3)],
        "renderSimulationVertexIndices": list(mesh.ids),
    }


def cloth_mesh(cloth, resource):
    """keen::RenderClothMesh: the resource and the tuning: gravity, damping and stiffness as asked, the rest FIXED_TUNING."""
    return {"data": resource, "gravity": float(cloth.gravity), "damping": float(cloth.damping), "phasesStiffness": float(cloth.stiffness), **FIXED_TUNING}
