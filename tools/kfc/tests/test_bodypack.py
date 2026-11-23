# SPDX-License-Identifier: MIT
"""Portable body lowering on synthetic bytes; optional reflection/roundtrip checks use the local game read-only."""
import copy
from contextlib import closing
import json
import os
import struct
import subprocess
import tempfile
import unittest
import uuid
from pathlib import Path

from test_questbuild import GAME, OUT, compile_cli
from kfc.util import fnv1a32, guid_bytes

MODEL = "3cfc8711-f659-5b14-873d-430c16e9c536"
GRAPH = "3c61aa14-09ef-590f-a662-c95fd33f0a36"
REMOVED = {"CharacterSetup": 44, "NewCharacterSetup": 1, "RenderEquipment": 788, "EquipmentCheck": 1, "SlotSelection": 216}
SIZES = dict(REMOVED, ModelResource=16, AnimationGraphResourceComponent2=40, AnimationGraphComponent2=44,
             PathWalker=92, MovementColliderConfig=32, UiOffsets=80, ColliderResourceComponent=8,
             AnimationGraphSlotComponent=1760, ServerAnimationGraphSlotComponent=1760,
             ActiveNpcState=456, LocaTagComponent=16, NpcDialog=120, SyntheticNested=8)
SHAPES = [2, 1, 0, 0, 0, 0, 0, 2, 2, 2, 2]


def u32(b, at):
    return struct.unpack_from("<I", b, at)[0]


def arr(b, field):
    rel, count = struct.unpack_from("<iI", b, field)
    return field + rel, count


def setarr(b, field, at, count):
    struct.pack_into("<iI", b, field, at - field if count else 0, count)


def allocate(b, size):
    b.extend(b"\0" * (-len(b) % 8))
    at = len(b)
    b.extend(b"\0" * size)
    return at


def variants(b, field):
    at, count = arr(b, field)
    return [(u32(b, h), h + 4 + struct.unpack_from("<i", b, h + 4)[0], u32(b, h + 8), h) for h in range(at, at + count * 12, 12)]


def components(b):
    return {kind: (at, size, header) for kind, at, size, header in variants(b, 16)}


def component(b, name):
    return components(b)[fnv1a32("keen::ecs::" + name)][0]


def fixture(omit=None):
    entries = [(name, size) for name, size in SIZES.items() if name != omit]
    b = bytearray(24 + len(entries) * 12)
    setarr(b, 16, 24, len(entries))
    for i, (name, size) in enumerate(entries):
        at = allocate(b, size)
        # Nonzero opaque bytes make accidental clearing or movement visible.
        b[at:at + size] = bytes((i * 11 + k) % 256 for k in range(size))
        h = 24 + i * 12
        struct.pack_into("<IiI", b, h, fnv1a32("keen::ecs::" + name), at - h - 4, size)
    child = allocate(b, 12)
    struct.pack_into("<III", b, child, 17, 23, 29)
    setarr(b, component(b, "SyntheticNested"), child, 3)
    modes = allocate(b, 11 * 56)
    setarr(b, component(b, "ColliderResourceComponent"), modes, 11)
    shapes = []
    for i, n in enumerate(SHAPES):
        mode = modes + i * 56
        data = allocate(b, n * 12)
        setarr(b, mode, data, n)
        struct.pack_into("<I", b, mode + 20, 0x12340000 + i)  # untouched physics fields
        for j in range(n):
            box = j == 0
            size = 96 if box else 88
            at = allocate(b, size)
            h = data + j * 12
            struct.pack_into("<IiI", b, h, fnv1a32("keen::ecs::BoxColliderData" if box else "keen::ecs::SphereColliderData"), at - h - 4, size)
            struct.pack_into("<II", b, at, (1 << (13 if box else 11)) | 0x10, 0x20)
            struct.pack_into("<3f4f", b, at + 8, 9, 8, 7, .1, .2, .3, .4)
            struct.pack_into("<I", b, at + 52, 0x12345678)  # material/feedback retained
            struct.pack_into("<3f" if box else "<f", b, at + 80, *((10, 20, 30) if box else (10,)))
            shapes.append((box, at, h))
    return b, shapes


class BodyPack(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = Path(os.environ["KILN_QUESTBUILD_CLI"]) if os.environ.get("KILN_QUESTBUILD_CLI") else compile_cli()

    def run_body(self, raw, spec=None):
        cfg = {"model": MODEL, "graph": GRAPH, "size": {"radius": .75, "height": 2}}
        if spec:
            cfg.update(spec)
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            d = Path(folder)
            (d / "input.bin").write_bytes(raw)
            (d / "spec.json").write_text(json.dumps(cfg))
            r = subprocess.run([str(self.exe), "--body", str(d / "input.bin"), "--body-spec", str(d / "spec.json"), "--out", str(d)], capture_output=True, text=True)
            self.assertIn(r.returncode, (0, 1), r.stderr)
            report = json.loads(r.stdout)
            self.assertTrue(report["inputUnchanged"])
            return report, (d / "body.bin").read_bytes()

    def test_only_selected_fields_change_and_all_nested_targets_keep_their_offsets(self):
        raw, shapes = fixture()
        before = bytes(raw)
        report, result = self.run_body(raw)
        self.assertTrue(report["ok"], report)
        old, new = components(raw), components(result)
        self.assertEqual(set(old) - {fnv1a32("keen::ecs::" + n) for n in REMOVED}, set(new))
        for kind, (at, size, _) in new.items():
            self.assertEqual((at, size), old[kind][:2])
        expected = bytearray(raw)
        expected[16:24] = result[16:24]  # root points to the appended retained-header array
        def put(name, offset, data):
            at = component(raw, name) + offset
            expected[at:at + len(data)] = data
        put("ModelResource", 0, guid_bytes(MODEL))
        put("AnimationGraphResourceComponent2", 0, guid_bytes(GRAPH))
        put("AnimationGraphComponent2", 8, b"\0" * 32)
        put("PathWalker", 56, struct.pack("<2f", .75, 2))
        put("MovementColliderConfig", 0, struct.pack("<2f", 1.5, 2))
        for name in ("AnimationGraphSlotComponent", "ServerAnimationGraphSlotComponent"):
            put(name, 0, b"\0" * 8)
        for offset, height in ((0, 2.2), (40, 1.2)):
            put("UiOffsets", offset, struct.pack("<6f", 0, height, 0, 0, 0, 0))
        for box, at, _ in shapes:
            struct.pack_into("<3f", expected, at + 8, 0, 1 if box else 0, 0)
            if box:
                struct.pack_into("<4f", expected, at + 20, 0, 0, 0, 1)
            struct.pack_into("<3f" if box else "<f", expected, at + 80, *((.75, 1, .75) if box else (.75,)))
        self.assertEqual(expected, result[:len(expected)])
        self.assertEqual(before, bytes(raw))
        self.assertGreater(len(result), len(raw))

    def test_a_missing_model_component_is_inserted_without_moving_other_payloads(self):
        raw, _ = fixture(omit="ModelResource")
        report, result = self.run_body(raw)
        self.assertTrue(report["ok"], report)
        self.assertEqual(guid_bytes(MODEL), result[component(result, "ModelResource"):component(result, "ModelResource") + 16])
        for kind, (at, size, _) in components(raw).items():
            if kind not in {fnv1a32("keen::ecs::" + n) for n in REMOVED}:
                self.assertEqual((at, size), components(result)[kind][:2])

    def test_identity_loca_and_dialogue_edits_run_after_the_body_rewrite(self):
        raw, _ = fixture()
        # edit_template reads the four dialogue line arrays even when only a headline changes.
        for offset in (0x34, 0x3c, 0x44, 0x4c):
            setarr(raw, component(raw, "NpcDialog") + 0x14 + offset, 0, 0)
        loca = "9494b53a-3b38-5ab9-83ce-714b333f598d"
        edit = {"name": "Kiln_synthetic_tobin", "npcId": 123456, "locaTag": loca, "headline": 98765}
        report, result = self.run_body(raw, {"edit": edit})
        self.assertTrue(report["ok"], report)
        name, count = arr(result, 0)
        self.assertEqual(edit["name"].encode(), result[name:name + count])
        self.assertEqual(edit["npcId"], u32(result, component(result, "ActiveNpcState")))
        self.assertEqual(guid_bytes(loca), result[component(result, "LocaTagComponent"):component(result, "LocaTagComponent") + 16])
        self.assertEqual(edit["headline"], u32(result, component(result, "NpcDialog") + 0x14))
        self.assertEqual(guid_bytes(GRAPH), result[component(result, "AnimationGraphResourceComponent2"):component(result, "AnimationGraphResourceComponent2") + 16])

    def test_changed_or_truncated_chassis_refuses_without_replacing_output(self):
        base, shapes = fixture()
        mutations = {
            "short": lambda b: b.__delitem__(slice(0, len(b) - 23)),
            "truncated": lambda b: b.pop(),
            "component count": lambda b: struct.pack_into("<I", b, 20, 513),
            "component target": lambda b: struct.pack_into("<i", b, 24 + 4, -2147483648),
            "duplicate": lambda b: struct.pack_into("<I", b, 36, u32(b, 24)),
            "size": lambda b: struct.pack_into("<I", b, components(b)[fnv1a32("keen::ecs::PathWalker")][2] + 8, 91),
            "modes": lambda b: struct.pack_into("<I", b, component(b, "ColliderResourceComponent") + 4, 10),
            "shape type": lambda b: struct.pack_into("<I", b, shapes[0][2], fnv1a32("keen::ecs::CapsuleColliderData")),
            "shape flags": lambda b: struct.pack_into("<I", b, shapes[0][1], 0),
            "shape target": lambda b: struct.pack_into("<i", b, shapes[0][2] + 4, 2147483647),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                raw = bytearray(base)
                mutate(raw)
                report, result = self.run_body(raw)
                self.assertFalse(report["ok"])
                self.assertTrue(report["reason"])
                self.assertEqual(b"Z", result)
        for missing in ("CharacterSetup", "PathWalker", "AnimationGraphComponent2"):
            with self.subTest(missing=missing):
                raw, _ = fixture(omit=missing)
                report, result = self.run_body(raw)
                self.assertFalse(report["ok"])
                self.assertIn(missing, report["reason"])
                self.assertEqual(b"Z", result)

    @unittest.skipUnless((GAME / "enshrouded.exe").is_file(), "no installed game reflection")
    def test_local_game_layouts_and_every_decoded_unrelated_field_are_preserved(self):
        from kfc.archive import Kfc, ResourceKey
        from kfc.blob import Decoder
        from kfc.refl import TypeDb
        layouts = json.loads(subprocess.run([str(self.exe), "--body-layout"], capture_output=True, text=True, check=True).stdout)
        self.assertGreater(len(layouts), 20)
        server = Path.home() / ".local/share/Steam/steamapps/common/EnshroudedServer"
        installs = [(GAME, "enshrouded.exe")]
        if (server / "enshrouded_server.exe").is_file():
            installs.append((server, "enshrouded_server.exe"))
        for directory, exe in installs:
            with self.subTest(side=exe):
                types = TypeDb.from_exe(directory / exe)
                for layout in layouts:
                    actual = types.get(layout["name"])
                    self.assertIsNotNone(actual, layout["name"])
                    self.assertEqual(layout["size"], actual["size"], layout["name"])
                    if layout["inner"]:
                        self.assertEqual(layout["inner"], actual["inner"], layout["name"])
                    for field in layout["fields"]:
                        self.assertIn(field, actual.get("fields", []), layout["name"])
                    for value in layout["values"]:
                        self.assertIn(value, actual.get("values", []), layout["name"])
                with closing(Kfc(directory)) as archive:
                    raw = archive.read(ResourceKey("e7666cd3-e530-4f9d-9586-519e4ab969dc", fnv1a32("keen::ecs::TemplateResource"), 0))
                report, result = self.run_body(raw)
                self.assertTrue(report["ok"], report)
                decoder = Decoder(types, max_array=1 << 24)
                expected = copy.deepcopy(decoder.decode("keen::ecs::TemplateResource", raw))
                expected["components"] = [c for c in expected["components"] if c["$type"].split("::")[-1] not in REMOVED]
                for c in expected["components"]:
                    name, v = c["$type"].split("::")[-1], c["value"]
                    if name == "ModelResource": v["model"] = MODEL
                    if name == "AnimationGraphResourceComponent2": v["animationGraph"] = GRAPH
                    if name == "AnimationGraphComponent2":
                        v["clothCollider"] = v["currentClothCollider"] = str(uuid.UUID(int=0))
                    if name in ("AnimationGraphSlotComponent", "ServerAnimationGraphSlotComponent"): v["slotsInitialized"] = []
                    if name == "PathWalker": v.update(horizontalRadius=.75, height=2)
                    if name == "MovementColliderConfig": v.update(width=1.5, height=2)
                    if name == "UiOffsets":
                        for key, height in (("healthBarOffset", 2.2), ("interactionPromptOffset", 1.2)):
                            v[key]["localOffset"] = {"x": 0, "y": height, "z": 0}
                            v[key]["worldOffset"] = {"x": 0, "y": 0, "z": 0}
                    if name == "ColliderResourceComponent":
                        for mode in v["colliders"]:
                            for shape in mode["dataArray"]:
                                data = shape["value"]
                                box = shape["$type"].endswith("BoxColliderData")
                                data["offset"] = {"x": 0, "y": 1 if box else 0, "z": 0}
                                if box:
                                    data["shape"]["halfSize"] = {"x": .75, "y": 1, "z": .75}
                                    data["orientation"] = {"x": 0, "y": 0, "z": 0, "w": 1}
                                else: data["shape"]["radius"] = .75
                def same(a, b):
                    if isinstance(a, dict):
                        self.assertEqual(set(a), set(b))
                        for key in a: same(a[key], b[key])
                    elif isinstance(a, list):
                        self.assertEqual(len(a), len(b))
                        for x, y in zip(a, b): same(x, y)
                    elif isinstance(a, float): self.assertAlmostEqual(a, b, delta=max(1e-6, abs(a) * 2e-7))
                    else: self.assertEqual(a, b)
                same(expected, decoder.decode("keen::ecs::TemplateResource", result))

    @unittest.skipUnless((GAME / "enshrouded.exe").is_file(), "no installed game data")
    def test_local_game_fisher_motion_closure_clones_and_remaps_every_reference(self):
        # 16f S4 on the game's own Fisher template and its 21 sequences (read only, nothing kept): every animation event
        # bound in order, local times inside the retimed sub-sequences, every template reference a clone, no source left.
        from kfc.archive import Kfc, ResourceKey
        from kfc.blob import Decoder
        from kfc.refl import TypeDb
        closure = json.loads(subprocess.run([str(self.exe), "--motion-closure"], capture_output=True, text=True, check=True).stdout)
        self.assertEqual(21, len(closure))
        self.assertEqual(27, sum(c["variants"] for c in closure))
        rows = []
        for c in closure:
            for v in range(c["variants"]):
                rows.append({"role": c["role"], "variant": v, "state": str(uuid.uuid5(uuid.NAMESPACE_URL, f"kiln-test:{c['role']}:{v}")),
                             "durationNs": 1_500_000_000 + 1000 * len(rows), "loop": c["role"] in ("sit", "sleep") and v == 1, "clamp": False})
        bound = {}
        for row in rows:
            bound.setdefault(row["role"], []).append(row["state"])
        server = Path.home() / ".local/share/Steam/steamapps/common/EnshroudedServer"
        installs = [(GAME, "enshrouded.exe")] + ([(server, "enshrouded_server.exe")] if (server / "enshrouded_server.exe").is_file() else [])
        for directory, exe in installs:
            with self.subTest(side=exe):
                types = TypeDb.from_exe(directory / exe)
                decoder = Decoder(types, max_array=1 << 24)
                with tempfile.TemporaryDirectory(dir=OUT) as folder, closing(Kfc(directory)) as archive:
                    d = Path(folder)
                    (d / "template.bin").write_bytes(archive.read(ResourceKey("e7666cd3-e530-4f9d-9586-519e4ab969dc", fnv1a32("keen::ecs::TemplateResource"), 0)))
                    for c in closure:
                        (d / f"{c['guid']}.bin").write_bytes(archive.read(ResourceKey(c["guid"], fnv1a32("keen::actor::ActorSequenceResource"), 0)))
                    (d / "bindings.json").write_text(json.dumps(rows))
                    r = subprocess.run([str(self.exe), "--motion", str(d)], capture_output=True, text=True)
                    self.assertIn(r.returncode, (0, 1), r.stderr)
                    report = json.loads(r.stdout)
                    self.assertTrue(report["ok"], report)
                    clones = report["clones"]
                    events = 0
                    for c in closure:
                        src = decoder.decode("keen::actor::ActorSequenceResource", (d / f"{c['guid']}.bin").read_bytes())
                        out = decoder.decode("keen::actor::ActorSequenceResource", (d / f"{c['guid']}.clone.bin").read_bytes())
                        self.assertEqual(clones[c["guid"]], out["resourceId"])
                        names = []
                        for s_sub, o_sub in zip(src["subSequences"], out["subSequences"], strict=True):
                            self.assertEqual([e["$type"] for e in s_sub["events"]], [e["$type"] for e in o_sub["events"]])
                            self.assertEqual(s_sub["loop"], o_sub["loop"])
                            length = o_sub["length"]["value"]
                            for e in o_sub["events"]:
                                v = e["value"]
                                self.assertGreaterEqual(v["time"]["value"], 0)
                                self.assertLessEqual(v["time"]["value"] + v["duration"]["value"], length)
                                if e["$type"].endswith("::SetAnimationEvent"):
                                    names.append(v["animationName"])
                        self.assertEqual(bound[c["role"]], names, c["role"])
                        events += len(names)
                    self.assertEqual(27, events)
                    text = json.dumps(decoder.decode("keen::ecs::TemplateResource", (d / "template.clone.bin").read_bytes()))
                    for c in closure:
                        self.assertNotIn(c["guid"], text)
                    self.assertEqual(report["replaced"], sum(text.count(g) for g in clones.values()))
                    if exe == "enshrouded.exe":
                        self.assertEqual(83, report["replaced"])  # the 83 reference paths of chapter 16f's evidence

    def test_geometry_and_zero_ids_refuse_with_the_field(self):
        raw, _ = fixture()
        for spec, field in (({"model": str(uuid.UUID(int=0))}, "body.model"),
                            ({"graph": str(uuid.UUID(int=0))}, "animation"),
                            ({"size": {"radius": .01, "height": 2}}, "body.size.radius"),
                            ({"size": {"radius": .75, "height": 9}}, "body.size.height")):
            with self.subTest(spec=spec):
                report, result = self.run_body(raw, spec)
                self.assertFalse(report["ok"])
                self.assertIn(field, report["reason"])
                self.assertEqual(b"Z", result)
