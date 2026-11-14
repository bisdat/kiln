# SPDX-License-Identifier: MIT
"""Build a keen::RenderModel resource + its renderData content blob from triangles (format: docs/re/models.md).

Output layout (as the game's own cooked models):
    renderData = [group 0: P | N | T | C | UV streams][group 1 ...] [skin stream, when skinned] [blend shapes, when it has them] [u16 indices of every mesh, LOD 0 first]
    vertexData = {0, sum of group bytes}, skinningData = {after them, 8 x vertices}, blendShapeData = {after that}, indexData follows on 4 bytes, others empty.
Each glTF primitive (one material each) becomes one vertex group and one RenderModelMesh per LOD. Skinned primitives with the same vertices share a group, as the
game's LODs do. Blend shapes: per group, every vertex of the model's first shape, then of the next, 3 x f16 position deltas in metres. Every group has them, zeros where a
primitive has no such target, which is the layout most of the game's models use, and a group's blendShapeByteOffset is its start inside blendShapeData.
"""
import hashlib
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "kfc"))

from kfc import mesh as kmesh  # noqa: E402
from kfc.blobwrite import Encoder  # noqa: E402

from . import geometry as G, skin as S  # noqa: E402

# The value every vanilla RenderModel.renderData carries (10470 of 10470, client 1285601).
RENDER_DATA_CATEGORY = 1497973154
RENDER_DATA_OFFSET = 0x110  # RenderModel.renderData (ResourceContentReference; .hash is its first 16 bytes)
MAX_VERTICES = 65535  # u16 indices


class CookError(Exception):
    pass


# The layouts the cloth and chain writers are written for (docs/re/models.md section 9: client 26430625's reflection, the same
# as tests/fixtures/types-rendermodel.json): size, and offset and type of each field they write. A game whose types differ
# gets no physics from this cook (layout_problems); its rigid and skinned models cook as before.
PHYSICS_LAYOUTS = {
    "cloth": {
        "keen::RenderModel": (352, {"hierarchy": (0x80, "keen::ObjectReference<keen::ModelHierarchyResource>"), "globalScale": (0x9c, "keen::float"),
                                    "skinningMap": (0xd4, "keen::BlobArray<keen::uint16>"), "cloth": (0xdc, "keen::BlobOptional<keen::RenderModelClothData>"),
                                    "collider": (0xe4, "keen::RenderModelClothColliderData"), "sleeves": (0x100, "keen::BlobArray<keen::RenderModelClothSleeve>"),
                                    "jointCount": (0x108, "keen::uint16"), "hierarchyJointCount": (0x10a, "keen::uint16"), "renderData": (0x110, "keen::ResourceContentReference")}),
        "keen::RenderModelMesh": (56, {"vertexCount": (0x14, "keen::uint32"), "indexOffset": (0x18, "keen::uint32"), "indexCount": (0x1c, "keen::uint32"),
                                       "vertexDataByteOffset": (0x28, "keen::uint32"), "skinVertexOffset": (0x30, "keen::uint32"), "vertexFormat": (0x34, "keen::RenderVertexFormat"),
                                       "vertexStride": (0x35, "keen::uint8"), "clothMeshIndex": (0x36, "keen::uint8")}),
        "keen::RenderModelClothData": (20, {"hasSkinning": (0x0, "keen::bool"), "meshes": (0x4, "keen::BlobArray<keen::RenderClothMesh>"),
                                            "totalSimulationTriangleCount": (0xc, "keen::uint32"), "totalRenderVertexCount": (0x10, "keen::uint32")}),
        "keen::RenderClothMesh": (232, {"data": (0x0, "keen::RenderClothMeshResource"), "gravity": (0xa4, "keen::float32"), "windStrength": (0xa8, "keen::float32"),
                                        "dragCoefficient": (0xac, "keen::float32"), "liftCoefficient": (0xb0, "keen::float32"), "damping": (0xb4, "keen::float32"),
                                        "linearInertia": (0xb8, "keen::float32"), "angularInertia": (0xbc, "keen::float32"), "centrifugalInertia": (0xc0, "keen::float32"),
                                        "tetherStiffness": (0xc4, "keen::float32"), "phasesStiffness": (0xc8, "keen::float32"), "phasesCompressionLimit": (0xcc, "keen::float32"),
                                        "phasesStretchLimit": (0xd0, "keen::float32"), "motionConstraintsStiffness": (0xd4, "keen::float32"), "collisionFriction": (0xd8, "keen::float32"),
                                        "solverFrequency": (0xdc, "keen::float32"), "motionConstraintsUnlimitedRadius": (0xe0, "keen::float32"),
                                        "motionConstraintsMaxRadius": (0xe4, "keen::float32")}),
        "keen::RenderClothMeshResource": (164, {"fabric": (0x0, "keen::RenderClothFabricResource"), "simulationPositions": (0x44, "keen::BlobArray<keen::float3>"),
                                                "simulationInvMasses": (0x4c, "keen::BlobArray<keen::float32>"), "simulationStiffnesses": (0x54, "keen::BlobArray<keen::float32>"),
                                                "simulationVertexWeights": (0x5c, "keen::BlobArray<keen::float32>"), "simulationTriangles": (0x64, "keen::BlobArray<keen::uint3>"),
                                                "simulationBlendshapeDeltas": (0x6c, "keen::BlobArray<keen::uint16>"),
                                                "simulationSkinningData": (0x74, "keen::BlobArray<keen::RenderClothVertexSkinningData>"),
                                                "skinnedVertexIndices": (0x7c, "keen::BlobArray<keen::uint16>"), "renderPositions": (0x84, "keen::BlobArray<keen::float3>"),
                                                "renderUvs": (0x8c, "keen::BlobArray<keen::float2>"), "renderTriangles": (0x94, "keen::BlobArray<keen::uint3>"),
                                                "renderSimulationVertexIndices": (0x9c, "keen::BlobArray<keen::uint16>")}),
        "keen::RenderClothFabricResource": (68, {"numParticles": (0x0, "keen::uint32"), "phaseIndices": (0x4, "keen::BlobArray<keen::uint32>"),
                                                 "sets": (0xc, "keen::BlobArray<keen::uint32>"), "restValues": (0x14, "keen::BlobArray<keen::float>"),
                                                 "stiffnessValues": (0x1c, "keen::BlobArray<keen::float>"), "indices": (0x24, "keen::BlobArray<keen::uint32>"),
                                                 "anchors": (0x2c, "keen::BlobArray<keen::uint32>"), "tetherLengths": (0x34, "keen::BlobArray<keen::float>"),
                                                 "triangles": (0x3c, "keen::BlobArray<keen::uint32>")}),
        "keen::RenderModelClothColliderData": (28, {"hasSkinning": (0x0, "keen::bool"), "ignoreClothCollider": (0x1, "keen::bool"),
                                                    "spheres": (0x4, "keen::BlobArray<keen::RenderClothColliderSphere>"),
                                                    "capsules": (0xc, "keen::BlobArray<keen::RenderClothColliderCapsule>"),
                                                    "instances": (0x14, "keen::BlobArray<keen::RenderModelColliderInstanceData>")}),
        "keen::RenderClothColliderSphere": (24, {"center": (0x0, "keen::float3"), "radius": (0xc, "keen::float32"), "skinningData": (0x10, "keen::RenderClothColliderSkinningData")}),
        "keen::RenderClothColliderCapsule": (40, {"center0": (0x0, "keen::float3"), "radius0": (0xc, "keen::float32"), "center1": (0x10, "keen::float3"),
                                                  "radius1": (0x1c, "keen::float32"), "skinningData": (0x20, "keen::RenderClothColliderSkinningData")}),
        "keen::RenderClothColliderSkinningData": (8, {"jointIndices": (0x0, "keen::StaticArray<keen::uint8,4>"), "jointWeights": (0x4, "keen::StaticArray<keen::uint8,4>")}),
        "keen::RenderClothVertexSkinningData": (8, {"jointIndices": (0x0, "keen::StaticArray<keen::uint8,4>"), "jointWeights": (0x4, "keen::StaticArray<keen::uint8,4>")}),
        "keen::RenderModelColliderInstanceData": (16, {"sphereIndices": (0x0, "keen::BlobArray<keen::uint8>"), "capsuleIndices": (0x8, "keen::BlobArray<keen::uint8>")}),
    },
    "chains2": {
        "keen::RenderModel": (352, {"hierarchy": (0x80, "keen::ObjectReference<keen::ModelHierarchyResource>"), "globalScale": (0x9c, "keen::float"),
                                    "skinningMap": (0xd4, "keen::BlobArray<keen::uint16>"), "chain": (0xe0, "keen::BlobOptional<keen::RenderModelChainData>"),
                                    "jointCount": (0x108, "keen::uint16"), "hierarchyJointCount": (0x10a, "keen::uint16")}),
        "keen::RenderModelChainData": (104, {"chains": (0x0, "keen::BlobArray<keen::RenderModelChain>"), "chains2": (0x8, "keen::BlobArray<keen::RenderModelChain2>"),
                                             "configs": (0x10, "keen::BlobArray<keen::RenderModelChainConfig>"), "chainColliders": (0x18, "keen::BlobArray<keen::RenderModelColliderData>"),
                                             "anchorHierarchyJoints": (0x20, "keen::BlobArray<keen::uint8>"), "anchorDefaultPose": (0x28, "keen::BlobArray<keen::Matrix43>"),
                                             "anchorRestPosition": (0x30, "keen::BlobArray<keen::float3>"), "joints": (0x38, "keen::BlobArray<keen::uint8>"),
                                             "modelMatrices": (0x40, "keen::BlobArray<keen::Matrix43>"), "skinToJointMatrix": (0x48, "keen::BlobArray<keen::Matrix43>"),
                                             "restPosition": (0x50, "keen::BlobArray<keen::float3>"), "constraintJoints": (0x58, "keen::BlobArray<keen::uint8>"),
                                             "constraints": (0x60, "keen::BlobArray<keen::float3>")}),
        "keen::RenderModelChain2": (140, {"propagateRotation": (0x0, "keen::bool"), "gravity": (0x4, "keen::float32"), "config": (0x8, "keen::RenderModelChainConfig2"),
                                          "guid": (0x74, "keen::Guid"), "colliderIndex": (0x84, "keen::uint8"), "jointBeginIndex": (0x86, "keen::uint16"),
                                          "jointEndIndex": (0x88, "keen::uint16")}),
        "keen::RenderModelChainConfig2": (108, {"stiffness": (0x0, "keen::float"), "bendStiffness": (0x4, "keen::float"), "stretchLimit": (0x8, "keen::float"),
                                                "inverseMass": (0xc, "keen::float"), "damping": (0x10, "keen::float"), "friction": (0x14, "keen::float"),
                                                "linearInertiaHorizontal": (0x18, "keen::uint16"), "linearInertiaVertical": (0x1a, "keen::uint16"),
                                                "angularInertia": (0x1c, "keen::float"), "centrifugalInertia": (0x20, "keen::float"), "dragCoefficient": (0x24, "keen::float"),
                                                "restCurvatureScale": (0x28, "keen::float"), "compressionLimit": (0x2c, "keen::float"),
                                                "restPosition": (0x30, "keen::BlobArray<keen::uint32>"), "motionRadius": (0x38, "keen::StaticArray<keen::float,7>"),
                                                "extentRestPosition": (0x54, "keen::uint16"), "extentMaxAllowedDist": (0x56, "keen::uint16"),
                                                "particleRadius": (0x58, "keen::uint16"), "bendStiffnessTipScale": (0x5a, "keen::uint8"), "gravityScale": (0x5b, "keen::uint8"),
                                                "angleLimitCos": (0x5c, "keen::StaticArray<keen::uint16,7>"), "count": (0x6a, "keen::uint16")}),
        "keen::Matrix43": (64, {"rot": (0x0, "keen::Matrix33"), "pos": (0x30, "keen::Vector3")}),
    },
}


def layout_problems(types, group):
    """What differs between this game's reflected types and the layouts the `group` writer ("cloth", "chains2") is written for: [] when nothing."""
    out = []
    for name, (size, fields) in PHYSICS_LAYOUTS[group].items():
        t = types.get(name)
        if t is None:
            out.append(f"{name} is not a type of this game")
            continue
        if t["size"] != size:
            out.append(f"{name} has {t['size']} bytes, the cook writes {size}")
        have = {f["name"]: (f["offset"], f["type"]) for f in types.all_fields(t)}
        for fname, want in fields.items():
            got = have.get(fname)
            if got is None:
                out.append(f"{name}.{fname} is gone")
            elif got != want:
                out.append(f"{name}.{fname} is {got[1]} at +{got[0]:#x}, the cook writes {want[1]} at +{want[0]:#x}")
    return out


def content_hash(data):
    """kiln.assets' ContentHash for a mod blob: {size, sha256[0:4], [4:8], [8:12]} (little-endian u32s)."""
    d = hashlib.sha256(data).digest()
    return (len(data),) + struct.unpack_from("<3I", d)


def prepare(prim):
    """Fill missing attributes (glTF space), then convert to game space."""
    P = prim["positions"]
    if len(P) > MAX_VERTICES:
        raise CookError(f"primitive with {len(P)} vertices: split it (u16 indices, at most {MAX_VERTICES})")
    if len(prim["indices"]) % 3:
        raise CookError("index count is not a multiple of 3")
    if any(i >= len(P) for i in prim["indices"]):
        raise CookError("index out of range")
    p = dict(prim)
    if not p.get("normals"):
        p["normals"] = G.compute_normals(P, p["indices"])
    if not p.get("uvs"):
        p["uvs"] = [(0.0, 0.0)] * len(P)
    if not p.get("tangents"):
        p["tangents"] = G.compute_tangents(P, p["normals"], p["uvs"], p["indices"])
    if not p.get("colors"):
        p["colors"] = [(1.0, 1.0, 1.0, 1.0)] * len(P)
    return G.to_game(p)


def quantization(all_positions):
    """positionOffset/positionScale the way the game's cooker picks them (checked on vanilla models: the bounding
    box grown on every side by 20 % of its largest half extent)."""
    mn, mx = G.bounds(all_positions)
    half = max((mx[i] - mn[i]) / 2 for i in range(3))
    m = 0.2 * half if half > 0 else 0.01
    off = tuple(mn[i] - m for i in range(3))
    sc = tuple(mx[i] - mn[i] + 2 * m for i in range(3))
    return off, sc, mn, mx


def encode_group(p, off, sc):
    n = len(p["positions"])
    out = bytearray()
    for q in p["positions"]:
        out += struct.pack("<II", *kmesh.pack_pos21(q, sc, off))
    for v in p["normals"]:
        out += struct.pack("<I", kmesh.pack_1010102(v[0], v[1], v[2], 1))
    for v in p["tangents"]:
        out += struct.pack("<I", kmesh.pack_1010102(v[0], v[1], v[2], 1 if v[3] >= 0 else -1))
    for c in p["colors"]:
        out += bytes(max(0, min(255, int(round(x * 255)))) for x in c[:4])
    for uv in p["uvs"]:
        out += struct.pack("<ee", uv[0], uv[1])
    assert len(out) == 24 * n
    return bytes(out)


def encode_morphs(p, names):
    """One group's blend shape data, for the model's shapes `names` in order. A target the primitive lacks is zeros."""
    n = len(p["positions"])
    out = bytearray()
    for name in names:
        deltas = (p.get("morphs") or {}).get(name)
        if deltas is None:
            out += bytes(6 * n)
            continue
        if len(deltas) != n:
            raise CookError(f"morph target {name!r} has {len(deltas)} deltas for {n} vertices")
        for v in deltas:
            try:
                out += struct.pack("<3e", *v)
            except OverflowError as e:
                raise CookError(f"morph target {name!r}: a delta of {v} does not fit a 16-bit float") from e
    return bytes(out)


def _vec(v):
    return {"x": float(v[0]), "y": float(v[1]), "z": float(v[2])}


def cloth_block(meshes, primitives, collider, skinned):
    """What build() takes for a model with cloth: meshes, the keen::RenderClothMesh of each (cook/fabric.py), in cloth order; primitives,
    {LOD 0 primitive index: its cloth mesh (1-based)}; collider, the model's keen::RenderModelClothColliderData; skinned, whether the cloth
    follows the skeleton (simulation skin written)."""
    if not 1 <= len(meshes) <= 255 or sorted(primitives.values()) != list(range(1, len(meshes) + 1)):
        raise CookError(f"{len(meshes)} cloth meshes for primitives {primitives}: every cloth mesh is one primitive's, 1 to 255")
    data = {"hasSkinning": bool(skinned), "meshes": meshes,
            "totalSimulationTriangleCount": sum(len(m["data"]["simulationTriangles"]) for m in meshes),
            "totalRenderVertexCount": sum(len(m["data"]["renderPositions"]) for m in meshes)}
    return {"data": data, "primitives": dict(primitives), "collider": collider}


NO_COLLIDER = {"hasSkinning": False, "ignoreClothCollider": False, "spheres": [], "capsules": [], "instances": []}


def build(lods, materials, base, name, pixel_sizes=None, skin=None, morphs=None, cloth=None, chains=None):
    """lods: [[prepared primitive (game space), ...] per LOD]; materials: [{material guid, renderEffectId,
    materialFlags}] indexed by primitive["material_slot"]; base: a decoded vanilla RenderModel (dict) whose other
    fields are kept (empty cloth/sdf are forced). skin: {"hierarchy": skeleton GUID, "hierarchyJointCount", "globalScale"} for a skinned model,
    whose primitives carry "influences" (cook/skin.py). morphs: the blend shapes it has, by their keen::BlendShapeFlags name (skin.morph_names), whose deltas the
    primitives carry in "morphs". cloth: cloth_block()'s, for a model with cloth (one LOD): its primitives get their clothMeshIndex and a vertex
    group of their own. chains: {"data": keen::RenderModelChainData, "smap": skinningMap, "model_index": {skeleton joint: palette slot}}
    (cook/chains.py) for a model with bone chains: its palette replaces the plain sorted one. Returns (desc dict, renderData bytes)."""
    morphs = list(morphs or [])
    cloth_of = cloth["primitives"] if cloth else {}
    if cloth and len(lods) != 1:
        raise CookError(f"a model with cloth has one LOD ({len(lods)} given): the cloth's render-to-simulation map is for LOD 0")
    allp = [q for lod in lods for p in lod for q in p["positions"]]
    off, sc, mn, mx = quantization(allp)
    if chains and not skin:
        raise CookError("bone chains need a skinned model")
    smap = chains["smap"] if chains else S.joint_map(lods) if skin else []
    if len(smap) > S.MAX_MODEL_JOINTS:
        raise CookError(f"{len(smap)} joints carry weight: a model's skin stream addresses {S.MAX_MODEL_JOINTS} (u8)")
    model_index = chains["model_index"] if chains else {j: i for i, j in enumerate(smap)}
    groups = bytearray()
    skin_stream = bytearray()
    morph_stream = bytearray()
    known_groups = {}
    indices = []
    meshes = []
    lod_desc = []
    vcount_total = 0
    for li, lod in enumerate(lods):
        first = len(meshes)
        for pi, p in enumerate(lod):
            ci = cloth_of.get(pi, 0) if li == 0 else 0
            vb = encode_group(p, off, sc)
            sb = S.encode_stream(p["influences"], model_index) if skin else b""
            mb = encode_morphs(p, morphs)
            c, r = G.bounding_sphere(p["positions"])
            group = known_groups.get((vb, sb, mb)) if skin and not ci else None
            if group is None:
                group = (len(groups), vcount_total, len(morph_stream))
                if not ci:  # a cloth primitive's vertices are its own: the client moves them
                    known_groups[(vb, sb, mb)] = group
                groups += vb
                skin_stream += sb
                morph_stream += mb
                vcount_total += len(p["positions"])
            meshes.append({
                "boundingSphere": {"center": _vec(c), "radius": float(r)},
                "materialIndex": p["material_slot"],
                "vertexCount": len(p["positions"]),
                "indexOffset": len(indices),
                "indexCount": len(p["indices"]),
                "meshletOffset": 0, "meshletCount": 0,
                "vertexDataByteOffset": group[0],
                "blendShapeByteOffset": group[2],
                "skinVertexOffset": group[1],
                "vertexFormat": "PNTCT", "vertexStride": 24, "clothMeshIndex": ci,
                "positionQuantization": "x21y21z21_unorm",
            })
            indices += p["indices"]
        ps = pixel_sizes[li] if pixel_sizes else (0.0 if li == 0 else 0.01 * (1.6 ** li))
        lod_desc.append({"meshOffset": first, "meshCount": len(meshes) - first, "pixelSize": float(ps)})
    vdata = bytes(groups)
    sdata = bytes(skin_stream)
    mdata = bytes(morph_stream)
    mpad = bytes(-(len(vdata) + len(sdata) + len(mdata)) & 3)
    idata = struct.pack(f"<{len(indices)}H", *indices)
    blob = vdata + sdata + mdata + mpad + idata
    c, r = G.bounding_sphere(allp)
    radius = max((q[0] ** 2 + q[1] ** 2 + q[2] ** 2) ** 0.5 for q in allp)
    h = content_hash(blob)
    d = dict(base)
    d.update({
        "debugName": name,
        "boundingBox": {"min": _vec(mn), "max": _vec(mx)},
        "boundingSphere": {"center": _vec(c), "radius": float(r)},
        "positionScale": _vec(sc), "positionOffset": _vec(off),
        "wholeModelRadius": float(radius),
        "materials": materials,
        "meshlets": [], "meshes": meshes, "lods": lod_desc,
        "foliageAnimationShaderData": None,
        "hierarchy": "00000000-0000-0000-0000-000000000000",
        "blendShapes": [{"id": kmesh.BLEND_SHAPES[m]} for m in morphs], "blendShapeMask": (1 << len(morphs)) - 1, "globalScale": 1.0, "hasCardClusterLod": False,
        "sdf": {"parts": []},
        "skinnedTree": {"jointMatrix": [], "jointParent": [], "jointSpringData": [], "leafJoints": [],
                        "inverseTransformTree": []},
        "skinningMap": smap, "cloth": cloth["data"] if cloth else None, "chain": chains["data"] if chains else None,
        "collider": cloth["collider"] if cloth else dict(NO_COLLIDER),
        "sleeves": [], "jointCount": len(smap), "hierarchyJointCount": skin["hierarchyJointCount"] if skin else 0,
        "renderData": {"hash": {"size": h[0], "hash0": h[1], "hash1": h[2], "hash2": h[3]},
                       "category": {"value": RENDER_DATA_CATEGORY}, "bundleId": 0, "sortKey": 0},
        "indexData": {"start": len(vdata) + len(sdata) + len(mdata) + len(mpad), "count": len(idata)},
        "meshletData": {"start": (len(blob) + 3) & ~3 if skin else len(blob), "count": 0},
        "vertexData": {"start": 0, "count": len(vdata)},
        "skinningData": {"start": len(vdata) if skin else 0, "count": len(sdata)},
        "blendShapeData": {"start": len(vdata) + len(sdata) if mdata else 0, "count": len(mdata)},
    })
    if skin:
        d["hierarchy"] = skin["hierarchy"]
        d["globalScale"] = float(skin.get("globalScale", 1.0))
    return d, blob


def encode(types, desc):
    data = Encoder(types).encode("keen::RenderModel", desc)
    # the patch offset kiln.assets uses must hold the hash we computed
    h = desc["renderData"]["hash"]
    assert data[RENDER_DATA_OFFSET:RENDER_DATA_OFFSET + 16] == struct.pack("<4I", h["size"], h["hash0"], h["hash1"], h["hash2"])
    return data
