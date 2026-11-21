#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generator rules = ShroudForge's (select_resource_profile.cpp, parse_resource_layout.cpp, configure_engine_layout.cpp)."""
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate as g  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
PROFILES = {p.parent.name: json.loads(p.read_text()) for p in sorted((ROOT / 'sigdb/engine').glob('*/*.json'))}
TARGET = {'client': 'enshrouded.exe', 'server': 'enshrouded_server.exe'}


def run(profile, side):
    size = profile['image']['size']
    g.parse_resource_layout(profile, size)
    return g.configure(profile, size, TARGET[side])


class Rules(unittest.TestCase):
    def mutated(self, side, edit):
        p = copy.deepcopy(PROFILES[side])
        edit(p)
        return p

    def refuses(self, side, edit, message):
        with self.assertRaises(g.ProfileError) as caught:
            run(self.mutated(side, edit), side)
        self.assertIn(message, str(caught.exception))

    def test_ported_profiles_configure(self):
        for side, p in PROFILES.items():
            layout, sections = run(p, side)
            self.assertTrue(layout['ecs'] and layout['authority'] and layout['write']['enabled'], side)
            self.assertEqual(sections & 0x6f, 0x6f)  # resource_read is set by load() after parse_resource_layout
            self.assertEqual(layout['dedicated_server'], side == 'server')

    def test_eligibility(self):
        for side, p in PROFILES.items():
            self.assertTrue(g.eligible(p, TARGET[side]))
            self.assertFalse(g.eligible(p, TARGET['server' if side == 'client' else 'client']))
            for edit in (lambda q: q['provenance'].update(status='approved'),
                         lambda q: q['provenance'].update(resourceReadValidated=False),
                         lambda q: q['provenance'].pop('resourceReadValidated'),
                         lambda q: q.update(kfcFormat=3), lambda q: q.update(schemaVersion=3),
                         lambda q: q['provenance'].update(resourceReadValidated='yes')):
                self.assertFalse(g.eligible(self.mutated(side, edit), TARGET[side]))

    def test_provenance_flags_gate_sections(self):
        cases = [('ecsReadValidated', 'ECS owner layout lacks read validation'),
                 ('ecsAuthorityReadValidated', 'local authority owner layout lacks read validation'),
                 ('ecsWriteGateValidated', 'ECS write gate lacks original authority/lifetime validation'),
                 ('ecsChangeTrackingValidated', 'ECS writes lack change tracking validation'),
                 ('resourceAtomicScalarWriteValidated', 'resource scalar writes lack original worker lifetime validation'),
                 ('servicePlainDataValidated', 'named service data lacks original mutable/lifetime validation'),
                 ('systemDependenciesValidated', 'system dependencies lack original scheduler validation'),
                 ('itemIdMapValidated', 'unproven original ItemRegistry map contract')]
        for side in PROFILES:
            for flag, message in cases:
                self.refuses(side, lambda q, f=flag: q['provenance'].pop(f), message)

    def test_offset_bounds_and_types(self):
        for side in PROFILES:
            size = PROFILES[side]['image']['size']
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(threadCallbackRva=size),
                         'engine owner offset out of range: threadCallbackRva')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(sceneLockOffset=65537),
                         'engine owner offset out of range: sceneLockOffset')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(sceneLockOffset=-1),
                         'engine owner offset out of range: sceneLockOffset')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(sceneLockOffset='1496'), 'type must be number')
            self.refuses(side, lambda q: q['ecsWriteLayout'].pop('sceneLockOffset'), "key 'sceneLockOffset' not found")
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(threadRegistryLockRva=q['ecsWriteLayout']['threadRegistryLockRva'] + 2),
                         'invalid native-thread ECS write gate layout')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(changeTrackerBytes=q['ecsWriteLayout']['changeTrackerBytes'].upper()),
                         'invalid change tracker guard bytes')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(changeTrackerBytes='00'), 'invalid original ECS change tracker')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(threadName=''), 'invalid native-thread ECS write gate layout')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(phasePolicy='all-components'),
                         'ECS write phase policy must be untracked-components')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(enabled='true'), 'type must be boolean')
            self.refuses(side, lambda q: q['ecsReadLayout'].update(maximumEntities=0), 'engine entity limit is zero')
            self.refuses(side, lambda q: q['ecsReadLayout'].pop('entityManagerKeys'),
                         'ECS entity hash membership needs both occupancy and keys offsets')
            self.refuses(side, lambda q: q['ecsReadLayout'].update(sceneEntryStride=16), 'invalid active scene entry layout')
            self.refuses(side, lambda q: q.pop('buildingPropSpawnLayout'), 'version 2 profile requires explicit queue and spawn layouts')
            self.refuses(side, lambda q: q['entityCommandQueueLayout'].update(extra=1), 'invalid original entity command queue layout')
            self.refuses(side, lambda q: q['entityCommandQueueLayout'].update(taskRecordStride=8),
                         'invalid original entity command queue extent')
            self.refuses(side, lambda q: q['buildingPropSpawnLayout'].update(sourceEntityIdsOffset=0),
                         'invalid building prop spawn value: sourceEntityIdsOffset')
            self.refuses(side, lambda q: q['resourceWriteLayout'].update(serviceInitializedValue=2),
                         'invalid resource content worker layout')
            self.refuses(side, lambda q: q['resourceLayout']['map'].update(keyStride=16),
                         'unsupported ResourceService map record layout')
            self.refuses(side, lambda q: q['resourceLayout'].update(resourceServiceIdCallbackRva=0),
                         'incomplete ResourceService profile layout')
            self.refuses(side, lambda q: q.update(schemaVersion=1),
                         'explicit queue and spawn layouts require profile format version 2')

    def test_dedicated_server_needs_registered_scene(self):
        self.refuses('server', lambda q: q['ownerLayout'].update(rootObjectRva=16),
                     'dedicated server requires a validated registered Scene owner layout')

    def test_version1_legacy_queue(self):
        p = copy.deepcopy(PROFILES['client'])
        p['schemaVersion'] = 1
        p.pop('entityCommandQueueLayout')
        p.pop('buildingPropSpawnLayout')
        layout, _ = run(p, 'client')
        self.assertEqual(layout['queue']['task_state'], 0x538)
        self.assertEqual(layout['queue']['execution_mode'], layout['entity_manager_offset'] + 8)
        self.assertEqual(layout['spawn'], {'source_entity_ids': 2088, 'allocation_failure_marker': 0})

    def test_check_mode_is_clean(self):
        self.assertEqual(g.generate(ROOT), (ROOT / g.OUTPUT).read_text(encoding='utf-8'))


class AttributeModel(unittest.TestCase):
    """ShroudForge publish_attribute_model (loaded-resources/initialize_resource_access.cpp:30-38): the selected exact
    profile's attributeCalculationModel is published only as "attribute-command-v1" with non-empty string evidence."""

    def model(self, edit):
        p = copy.deepcopy(PROFILES['client'])
        edit(p)
        return g.attribute_model(p)

    def test_ported_profiles_publish_the_command_model(self):
        for side, p in PROFILES.items():
            self.assertEqual(g.attribute_model(p), 'attribute-command-v1', side)
        generated = (ROOT / g.OUTPUT).read_text(encoding='utf-8')
        self.assertEqual(generated.count('[0], "attribute-command-v1"},'), len(PROFILES))  # each Profile row's last field

    def test_evidence_must_be_a_non_empty_string(self):
        self.assertEqual(self.model(lambda p: p['provenance'].pop('attributeCalculationEvidence')), '')
        self.assertEqual(self.model(lambda p: p['provenance'].update(attributeCalculationEvidence='')), '')
        self.assertEqual(self.model(lambda p: p['provenance'].update(attributeCalculationEvidence=['x'])), '')

    def test_only_the_command_model_is_published(self):
        self.assertEqual(self.model(lambda p: p.update(attributeCalculationModel='attribute-command-v2')), '')
        self.assertEqual(self.model(lambda p: p.pop('attributeCalculationModel')), '')

    def test_malformed_inputs_refuse_the_profile(self):
        # ShroudForge: value() of a non-string and at("provenance") on a profile without one throw
        with self.assertRaises(g.ProfileError):
            self.model(lambda p: p.update(attributeCalculationModel=1))
        with self.assertRaises(g.ProfileError):
            self.model(lambda p: p.pop('provenance'))


class TypedResourceLayout(unittest.TestCase):
    """resourceLayout typed as ShroudForge's parse_layout produces it (parse_resource_layout.cpp:10-110,
    loaded_resource_state.h:40-65): every field in the struct's order, from the profile's own JSON."""

    # (C++ field of kiln::engine::ResourceLayout, path in resourceLayout), in the struct's order; written out here, not
    # taken from generate.py, so a wrong key or order there fails this test.
    FIELDS = [('main_active_context_offset', ('mainActiveContextOffset',)),
              ('context_item_registry_offset', ('contextItemRegistryOffset',)),
              ('item_registry_service_offset', ('itemRegistryServiceOffset',)),
              ('item_registry_handle_offset', ('itemRegistryHandleOffset',)),
              ('item_registry_resource_pointer_offset', ('itemRegistryResourcePointerOffset',)),
              ('resource_service_reader_offset', ('resourceServiceReaderOffset',)),
              ('resource_service_chunk_table_offset', ('resourceServiceChunkTableOffset',)),
              ('resource_service_id_callback_offset', ('resourceServiceIdCallbackOffset',)),
              ('resource_service_handle_callback_offset', ('resourceServiceHandleCallbackOffset',)),
              ('resource_service_id_callback_rva', ('resourceServiceIdCallbackRva',)),
              ('resource_service_handle_callback_rva', ('resourceServiceHandleCallbackRva',)),
              ('resource_service_chunk_count_offset', ('resourceServiceChunkCountOffset',)),
              ('reader_metadata_offset', ('readerMetadataOffset',)),
              ('metadata_map_offset', ('metadataMapOffset',)),
              ('bucket_rel32_offset', ('map', 'bucketRel32Offset')),
              ('bucket_count_offset', ('map', 'bucketCountOffset')),
              ('keys_rel32_offset', ('map', 'keysRel32Offset')),
              ('key_count_offset', ('map', 'keyCountOffset')),
              ('values_rel32_offset', ('map', 'valuesRel32Offset')),
              ('value_count_offset', ('map', 'valueCountOffset')),
              ('map_header_size', ('map', 'headerSize')),
              ('bucket_stride', ('map', 'bucketStride')),
              ('key_stride', ('map', 'keyStride')),
              ('resource_key_size', ('map', 'resourceKeySize')),
              ('value_stride', ('map', 'valueStride')),
              ('resource_offset_offset', ('valueEntry', 'resourceOffsetOffset')),
              ('packed_size_offset', ('valueEntry', 'packedSizeOffset')),
              ('chunk_index_offset', ('valueEntry', 'chunkIndexOffset')),
              ('size_mask', ('valueEntry', 'sizeMask')),
              ('chunk_stride', ('chunkEntry', 'stride')),
              ('chunk_base_pointer_offset', ('chunkEntry', 'basePointerOffset')),
              ('chunk_size_offset', ('chunkEntry', 'sizeOffset')),
              ('maximum_entries', ('maximumEntries',)),
              ('maximum_chunks', ('maximumChunks',)),
              ('maximum_resource_bytes', ('maximumResourceBytes',))]
    ITEM_FIELDS = [('type_hash', 'targetHash'), ('part_index', 'partIndex'), ('root_size', 'targetSize'),
                   ('item_id_offset', 'itemIdOffset'), ('guid_offset', 'guidOffset'), ('map_offset', 'mapOffset'),
                   ('header_size', 'headerSize'), ('bits_offset', 'bitsPointerOffset'),
                   ('capacity_offset', 'capacityOffset'), ('keys_offset', 'keysPointerOffset'),
                   ('values_offset', 'valuesPointerOffset'), ('count_offset', 'countOffset'),
                   ('maximum_capacity', 'maximumCapacity'), ('maximum_probes', 'maximumProbes')]

    @staticmethod
    def value(profile, path):
        node = profile['resourceLayout']
        for key in path:
            node = node[key]
        return node

    def expected(self, profile):
        """The ordered initializer values a correct generator emits for this profile."""
        values = [self.value(profile, path) for _, path in self.FIELDS]
        item = profile['resourceLayout'].get('itemIdMap')
        values.append(1 if item else 0)
        values.extend(item[key] if item else 0 for _, key in self.ITEM_FIELDS)
        return values

    def test_typed_fields_match_the_profile(self):
        for side, p in PROFILES.items():
            typed = g.parse_resource_layout(p, p['image']['size'])['typed']
            for field, path in self.FIELDS:
                self.assertEqual(typed[field], self.value(p, path), '%s %s' % (side, field))
            item = p['resourceLayout']['itemIdMap']
            self.assertTrue(typed['item_map']['enabled'], side)
            for field, key in self.ITEM_FIELDS:
                self.assertEqual(typed['item_map'][field], item[key], '%s item_map.%s' % (side, field))

    def test_reader_is_the_one_the_guard_checks(self):
        # Configure copies resourceServiceReaderOffset into the content worker layout (configure_engine_layout.cpp:260)
        for side, p in PROFILES.items():
            typed = g.parse_resource_layout(p, p['image']['size'])['typed']
            layout, _ = run(p, side)
            self.assertEqual(typed['resource_service_reader_offset'], layout['resource_write']['reader'], side)

    def test_without_an_item_map_the_record_is_zero(self):
        for side in PROFILES:
            p = copy.deepcopy(PROFILES[side])
            p['resourceLayout'].pop('itemIdMap')
            typed = g.parse_resource_layout(p, p['image']['size'])['typed']
            self.assertEqual(typed['item_map'], {'enabled': False, **{f: 0 for f, _ in self.ITEM_FIELDS}}, side)

    def test_each_profile_row_carries_its_record_last(self):
        import re
        generated = (ROOT / g.OUTPUT).read_text(encoding='utf-8')
        rows = [line for line in generated.splitlines() if 'Layout{' in line and 'ResourceWriteLayout{' in line]
        self.assertEqual(len(rows), len(PROFILES))
        for side, p in PROFILES.items():
            row = next(line for line in rows if '"%s"' % TARGET[side] in line)
            # the Layout initializer ends with ResourceWriteLayout{...}, ResourceLayout{..., ItemMapLayout{...}}, CallRecipes{}}
            found = re.search(r'ResourceWriteLayout\{[^{}]*\{[^{}]*\}\}, ResourceLayout\{([^{}]*), ItemMapLayout\{([^{}]*)\}\}, '
                              r'CallRecipes\{\}\}', row)
            self.assertIsNotNone(found, side)
            numbers = [int(v.strip().rstrip('u')) if v.strip() not in ('true', 'false') else int(v.strip() == 'true')
                       for v in (found.group(1) + ', ' + found.group(2)).split(',')]
            self.assertEqual(numbers, self.expected(p), side)

    # Every resourceLayout number ShroudForge's rules leave free gets a value no other field has, within those rules
    # (parse_resource_layout.cpp:15-105); the fixed ones (map strides, key size, size mask, chunk record, partIndex) keep
    # the only value parse_layout accepts. In both exact profiles several fields share a value (8, 32, 472, ...), so there a
    # field typed from another field's key, or emitted in another order, would produce the same bytes and pass.
    DISTINCT = {'mainActiveContextOffset': 1001, 'contextItemRegistryOffset': 1002, 'itemRegistryServiceOffset': 1003,
                'itemRegistryHandleOffset': 1004, 'itemRegistryResourcePointerOffset': 1005,
                'resourceServiceReaderOffset': 1006, 'resourceServiceIdCallbackOffset': 1007,
                'resourceServiceIdCallbackRva': 2001, 'resourceServiceHandleCallbackOffset': 1008,
                'resourceServiceHandleCallbackRva': 2002, 'resourceServiceChunkTableOffset': 1009,
                'resourceServiceChunkCountOffset': 1010, 'readerMetadataOffset': 1011, 'metadataMapOffset': 1012,
                'maximumEntries': 3001, 'maximumChunks': 3002, 'maximumResourceBytes': 0x100000,
                'map': {'bucketRel32Offset': 100, 'bucketCountOffset': 104, 'keysRel32Offset': 108,
                        'keyCountOffset': 112, 'valuesRel32Offset': 116, 'valueCountOffset': 120, 'headerSize': 0x800},
                'valueEntry': {'resourceOffsetOffset': 1, 'packedSizeOffset': 3, 'chunkIndexOffset': 5},
                'itemIdMap': {'targetHash': 0xabcdef, 'targetSize': 200, 'itemIdOffset': 12, 'guidOffset': 40,
                              'mapOffset': 4001, 'headerSize': 96, 'bitsPointerOffset': 9, 'capacityOffset': 10,
                              'keysPointerOffset': 11, 'valuesPointerOffset': 13, 'countOffset': 14,
                              'maximumCapacity': 5001, 'maximumProbes': 77}}

    def distinct(self, side):
        p = copy.deepcopy(PROFILES[side])
        for key, value in self.DISTINCT.items():
            if isinstance(value, dict):
                p['resourceLayout'][key].update(value)
            else:
                p['resourceLayout'][key] = value
        return p

    def test_each_free_field_is_typed_from_its_own_key(self):
        free = []
        for key, value in self.DISTINCT.items():
            free.extend(value.values() if isinstance(value, dict) else [value])
        self.assertEqual(len(free), len(set(free)), 'the free values are distinct')
        for side in PROFILES:
            p = self.distinct(side)
            typed = g.parse_resource_layout(p, p['image']['size'])['typed']
            for field, path in self.FIELDS:
                self.assertEqual(typed[field], self.value(p, path), '%s %s' % (side, field))
            for field, key in self.ITEM_FIELDS:
                self.assertEqual(typed['item_map'][field], p['resourceLayout']['itemIdMap'][key],
                                 '%s item_map.%s' % (side, field))

    def test_emit_order_is_the_struct_declaration_order(self):
        # The generator emits ResourceLayout{...} and ItemMapLayout{...} positionally, in RESOURCE_FIELDS and
        # ITEM_MAP_FIELDS order: both must be the declaration order in kiln_engine_layout.h, whatever the values.
        import re
        header = (ROOT / 'native/services/engine/include/kiln_engine_layout.h').read_text(encoding='utf-8')
        header = re.sub(r'//[^\n]*', '', header)

        def members(struct):
            body = re.search(r'struct %s \{(.*?)\};' % struct, header, re.S).group(1)
            names = []
            for statement in body.split(';'):
                statement = statement.strip()
                if statement:
                    declarators = re.sub(r'^[\w:<>, ]*?\s+(?=\w+\s*(,|$))', '', statement, count=1)
                    names.extend(d.strip() for d in declarators.split(','))
            return names

        self.assertEqual(members('ResourceLayout'), [f for f, _ in g.RESOURCE_FIELDS] + ['item_map'])
        self.assertEqual(members('ResourceLayout'), [f for f, _ in self.FIELDS] + ['item_map'])
        self.assertEqual(members('ItemMapLayout'), ['enabled'] + [f for f, _ in g.ITEM_MAP_FIELDS])
        self.assertEqual(members('ItemMapLayout'), ['enabled'] + [f for f, _ in self.ITEM_FIELDS])
        # The Layout ends with the typed resource record, then the engine-call recipes, the generator's last two
        self.assertEqual(members('Layout')[-2:], ['resource', 'calls'])


class CallRecipes(unittest.TestCase):
    """Ported profiles carry no reviewed engine call. Every Profile row's Layout ends with an empty CallRecipes{}
    record, and that record is the only thing the row gained (the rest is byte for byte the parent's file)."""

    def test_each_profile_row_ends_with_empty_call_recipes(self):
        generated = (ROOT / g.OUTPUT).read_text(encoding='utf-8')
        rows = [line for line in generated.splitlines() if line.startswith('    {"enshrouded-') and 'Layout{' in line]
        self.assertEqual(len(rows), len(PROFILES))
        for row in rows:
            self.assertEqual(row.count('CallRecipes{'), 1, row[:60])
            self.assertIn('}}, CallRecipes{}}, kLeaves', row, row[:60])

    def test_generated_text_has_no_recipe_data(self):
        generated = (ROOT / g.OUTPUT).read_text(encoding='utf-8')
        self.assertEqual(generated.count('CallRecipes{}'), len(PROFILES))
        self.assertNotIn('CallRecipe{', generated)
        self.assertNotIn('CallRange{', generated)


def export_tree(root):
    """An exported copy of the generator's inputs and output under `root`."""
    for source in sorted((ROOT / 'sigdb/engine').glob('*/*.json')):
        target = root / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    (root / g.OUTPUT).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / g.OUTPUT, root / g.OUTPUT)


class AlternateRoot(unittest.TestCase):
    """--root selects the whole tree: generated path comments are relative to it, never to the script's checkout."""

    def test_sibling_root_checks_clean(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'exported'
            export_tree(root)
            self.assertEqual(g.generate(root), (ROOT / g.OUTPUT).read_text(encoding='utf-8'))
            run = subprocess.run([sys.executable, '-I', str(Path(g.__file__).resolve()), '--check', '--root', str(root)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_nested_root_checks_clean(self):
        # A root below the script's own root: the path comments must not gain the nesting prefix.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'outer' / 'inner'
            export_tree(root)
            with mock.patch.object(g, 'ROOT', Path(temp) / 'outer'):
                self.assertEqual(g.generate(root), (ROOT / g.OUTPUT).read_text(encoding='utf-8'))


class EngineCalls(unittest.TestCase):
    """Kiln's own engineCalls section (kiln_engine_layout.h CallRecipe), the reviewed engine
    calls of an exact build, typed into Layout::calls. No ported profile carries one (each recipe lands with the service
    that calls it); these cases use copies of the real profiles with a made-up recipe."""

    @staticmethod
    def recipe(rid=1, ranges=((0x1000, 64), (0x2000, 32))):
        return {'id': rid, 'argBytes': [64, 0], 'resultBits': 32,
                'ranges': [{'rva': rva, 'size': size, 'original_sha256': '%064x' % (rva + size)} for rva, size in ranges],
                'review': {'sfSource': 'impact/submit_building_prop_spawn.cpp:602-683', 'sfInHold': True,
                           'locks': ['the entity manager lock (shared)'], 'allocators': [],
                           'threadState': {'mxcsr': 'default, not changed', 'engineTls': 'none read'},
                           'stackBuffersMovedToHeap': ['the 0xa0-byte command record'],
                           'boundedLoops': ['the 6-component copy (6)']}}

    def parse(self, calls, side='server', write=True):
        p = copy.deepcopy(PROFILES[side])
        p['engineCalls'] = calls
        return g.parse_engine_calls(p, p['image']['size'], write)

    def refuses(self, calls, message, **kw):
        with self.assertRaises(g.ProfileError) as caught:
            self.parse(calls, **kw)
        self.assertIn(message, str(caught.exception))

    def edited(self, edit, name='PropSpawnSubmit', rid=1):
        r = self.recipe(rid)
        edit(r)
        return {name: r}

    def test_ported_profiles_carry_none(self):
        for side, p in PROFILES.items():
            self.assertNotIn('engineCalls', p, side)
            self.assertEqual(g.parse_engine_calls(p, p['image']['size'], True), [None] * 8, side)
        self.assertEqual(self.parse({}), [None] * 8)

    def test_a_recipe_lands_in_its_slot(self):
        slots = self.parse({'ImpactSameEntity': self.recipe(3), 'PropSpawnSubmit': self.recipe(1)})
        self.assertEqual([i for i, s in enumerate(slots) if s], [0, 2])
        self.assertEqual(slots[2], {'name': 'ImpactSameEntity', 'id': 3, 'arg_bytes': [64, 0], 'result_bits': 32,
                                    'ranges': [(0x1000, 64, '%064x' % 0x1040), (0x2000, 32, '%064x' % 0x2020)]})

    def test_the_names_and_limits_are_the_headers(self):
        import re
        header = (ROOT / 'native/services/engine/include/kiln_engine_layout.h').read_text(encoding='utf-8')
        enum = re.search(r'enum class CallRecipeId : std::uint32_t \{([^}]*)\}', header)
        self.assertIsNotNone(enum)
        ids = {k: int(v) for k, v in re.findall(r'(\w+) = (\d+)', enum.group(1))}
        self.assertEqual(ids, {'PropSpawnSubmit': 1, 'PropDestroySubmit': 2, 'ImpactSameEntity': 3})
        self.assertEqual(ids, g.CALL_RECIPE_IDS)
        limits = re.search(r'kCallRecipes = (\d+), kCallRanges = (\d+), kCallArguments = (\d+);', header)
        self.assertEqual(tuple(map(int, limits.groups())), (g.CALL_RECIPES, g.CALL_RANGES, g.CALL_ARGUMENTS))
        self.assertIn('kCallCodeBytes = 64 * 1024;', header)
        self.assertEqual(g.CALL_CODE_BYTES, 65536)

    def test_a_range_over_64_kib(self):
        self.refuses({'PropSpawnSubmit': self.recipe(ranges=((0x1000, 65537),))}, 'ranges.0: range of 65537 bytes is over 64 KiB')
        self.assertTrue(self.parse({'PropSpawnSubmit': self.recipe(ranges=((0x1000, 65536),))})[0])

    def test_ranges_together_over_64_kib(self):
        self.refuses({'PropSpawnSubmit': self.recipe(ranges=((0x10000, 40960), (0x40000, 24577)))},
                     'engineCalls.PropSpawnSubmit: ranges total over 64 KiB')
        self.refuses({'PropSpawnSubmit': self.recipe(ranges=[(0x10000 * (i + 1), 2049) for i in range(32)])},
                     'ranges total over 64 KiB')
        self.assertTrue(self.parse({'PropSpawnSubmit': self.recipe(ranges=((0x10000, 40960), (0x40000, 24576)))})[0])
        self.assertTrue(self.parse({'PropSpawnSubmit': self.recipe(ranges=[(0x10000 * (i + 1), 2048) for i in range(32)])})[0])

    def test_overlapping_ranges(self):
        for second in ((0x1020, 16), (0x0ff0, 0x11), (0x1000, 64), (0x0f00, 0x200)):
            self.refuses({'PropSpawnSubmit': self.recipe(ranges=((0x1000, 64), second))}, 'ranges.1: range overlaps ranges.0')
        self.refuses({'PropSpawnSubmit': self.recipe(ranges=((0x1000, 64), (0x3000, 8), (0x103f, 1)))},
                     'ranges.2: range overlaps ranges.0')
        for adjacent in ((0x1040, 16), (0x0ff0, 0x10)):
            self.assertTrue(self.parse({'PropSpawnSubmit': self.recipe(ranges=((0x1000, 64), adjacent))})[0])

    def test_slot_and_id_must_agree(self):
        self.refuses({'PropSpawnSubmit': self.recipe(2)}, 'recipe slot/id mismatch')
        self.refuses({'PropDestroySubmit': self.recipe(1)}, 'recipe slot/id mismatch')
        self.refuses({'ImpactSameEntity': self.recipe(True)}, 'recipe slot/id mismatch')
        self.refuses({'ImpactSameEntity': self.recipe('3')}, 'recipe slot/id mismatch')
        self.refuses({'Teleport': self.recipe(4)}, 'unknown engine call recipe: Teleport')
        self.assertTrue(self.parse({'PropDestroySubmit': self.recipe(2)})[1])

    def test_unknown_and_missing_keys(self):
        self.refuses(self.edited(lambda r: r.update(address=0x1000)), 'engineCalls.PropSpawnSubmit: unknown key address')
        self.refuses(self.edited(lambda r: r['ranges'][1].update(entry=True)), 'ranges.1: unknown key entry')
        self.refuses(self.edited(lambda r: r['review'].update(approvedBy='x')), 'review: unknown key approvedBy')
        self.refuses(self.edited(lambda r: r['review']['threadState'].update(fpcw='x')), 'threadState: unknown key fpcw')
        for key in g.CALL_KEYS:
            self.refuses(self.edited(lambda r, k=key: r.pop(k)), 'missing key ' + key)
        for key in g.CALL_REVIEW_KEYS:
            self.refuses(self.edited(lambda r, k=key: r['review'].pop(k)), 'missing key ' + key)
        self.refuses(self.edited(lambda r: r['ranges'][0].pop('original_sha256')), 'missing key original_sha256')

    def test_field_rules(self):
        cases = [(lambda r: r.update(argBytes=[0] * 9), 'argBytes must be at most 8'),
                 (lambda r: r.update(argBytes=[-1]), 'argBytes must be at most 8'),
                 (lambda r: r.update(argBytes=[1 << 32]), 'argBytes must be at most 8'),
                 (lambda r: r.update(argBytes=[True]), 'argBytes must be at most 8'),
                 (lambda r: r.update(resultBits=12), 'resultBits must be one of'),
                 (lambda r: r.update(resultBits=128), 'resultBits must be one of'),
                 (lambda r: r.update(ranges=[]), 'ranges must hold 1..32'),
                 (lambda r: r.update(ranges=[{'rva': 0x1000 + 16 * i, 'size': 1, 'original_sha256': '0' * 64}
                                             for i in range(33)]), 'ranges must hold 1..32'),
                 (lambda r: r['ranges'][0].update(size=0), 'size > 0'),
                 (lambda r: r['ranges'][0].update(rva=-1), 'size > 0'),
                 (lambda r: r['ranges'][0].update(rva=1.5), 'size > 0'),
                 (lambda r: r['ranges'][0].update(rva=31318016), 'range outside the image'),
                 (lambda r: r['ranges'][0].update(rva=31318016 - 8, size=16), 'range outside the image'),
                 (lambda r: r['ranges'][0].update(original_sha256='A' * 64), 'original_sha256 must be 64 lowercase hex'),
                 (lambda r: r['ranges'][0].update(original_sha256='0' * 63), 'original_sha256 must be 64 lowercase hex'),
                 (lambda r: r['review'].update(sfSource=''), 'sfSource must name'),
                 (lambda r: r['review'].update(sfInHold=False), 'sfInHold must be true'),
                 (lambda r: r['review'].update(sfInHold='true'), 'sfInHold must be true'),
                 (lambda r: r['review'].update(locks='none'), 'review.locks must be a list'),
                 (lambda r: r['review'].update(allocators=[1]), 'review.allocators must be a list'),
                 (lambda r: r['review'].update(boundedLoops=['']), 'review.boundedLoops must be a list'),
                 (lambda r: r['review'].update(stackBuffersMovedToHeap=None), 'stackBuffersMovedToHeap must be a list'),
                 (lambda r: r['review'].update(threadState='default'), 'threadState must be an object'),
                 (lambda r: r['review']['threadState'].update(engineTls=''), 'threadState.engineTls must be')]
        for edit, message in cases:
            self.refuses(self.edited(edit), message)

    def test_section_shape_and_the_write_gate(self):
        self.refuses([self.recipe()], 'engineCalls must be an object')
        self.refuses({'PropSpawnSubmit': [1]}, 'engineCalls.PropSpawnSubmit must be an object')
        self.refuses({'PropSpawnSubmit': self.recipe()}, 'engineCalls need the enabled ECS write gate', write=False)
        self.assertEqual(self.parse({}, write=False), [None] * 8)

    def root_with(self, calls_by_side, edit_raw=None):
        """A temporary --root holding both ported profiles, an engineCalls section added per side."""
        tmp = Path(tempfile.mkdtemp(prefix='engine-calls-'))
        self.addCleanup(shutil.rmtree, tmp)
        for path in sorted((ROOT / 'sigdb/engine').glob('*/*.json')):
            p = json.loads(path.read_text())
            if path.parent.name in calls_by_side:
                p['engineCalls'] = calls_by_side[path.parent.name]
            out = tmp / 'sigdb/engine' / path.parent.name / path.name
            out.parent.mkdir(parents=True)
            raw = json.dumps(p, indent=2)
            out.write_text(edit_raw(raw) if edit_raw else raw)
        return tmp

    def test_a_duplicate_key_refuses_the_profile(self):
        tmp = self.root_with({'server': {'PropSpawnSubmit': self.recipe()}},
                             lambda raw: raw.replace('"engineCalls": {', '"engineCalls": {"PropSpawnSubmit": {}, ', 1))
        with self.assertRaises(g.ProfileError) as caught:
            g.generate(tmp)
        self.assertIn('26430253.json: engineCalls: duplicate key PropSpawnSubmit', str(caught.exception))
        tmp = self.root_with({'server': {'PropSpawnSubmit': self.recipe()}},
                             lambda raw: raw.replace('"sfInHold": true', '"sfInHold": false, "sfInHold": true', 1))
        with self.assertRaises(g.ProfileError) as caught:
            g.generate(tmp)
        self.assertIn('duplicate key sfInHold', str(caught.exception))

    def test_a_second_engine_calls_section_refuses_the_profile(self):
        # json keeps the last of two equal keys: a second section must not silently replace (or hide) the first
        for second in ('{}', json.dumps({'PropDestroySubmit': self.recipe(2)})):
            tmp = self.root_with({'server': {'PropSpawnSubmit': self.recipe()}},
                                 lambda raw, s=second: raw.replace('"engineCalls": {', '"engineCalls": %s, "engineCalls": {' % s, 1))
            with self.assertRaises(g.ProfileError) as caught:
                g.generate(tmp)
            self.assertIn('26430253.json: duplicate key engineCalls', str(caught.exception))

    @staticmethod
    def rows(text):
        return [line for line in text.splitlines() if not line.startswith('// sigdb/engine/')]

    def test_no_recipe_generates_the_same_rows(self):
        # an empty section, or none, emits CallRecipes{} and the same leaves: only the files' sha256 comments differ
        self.assertEqual(self.rows(g.generate(self.root_with({'server': {}, 'client': {}}))),
                         self.rows((ROOT / g.OUTPUT).read_text(encoding='utf-8')))

    def test_recipes_are_emitted_in_their_slots_and_compile(self):
        calls = {'ImpactSameEntity': self.recipe(3, ((0x1000, 64), (0x2000, 32), (0x5000, 1)))}
        text = g.generate(self.root_with({'server': calls}))
        self.assertIn(', CallRecipes{{CallRecipe{}, CallRecipe{}, CallRecipe{3u, 2u, 32u, 3u, {64u, 0u, 0u, 0u, 0u, 0u, '
                      '0u, 0u}, {CallRange{0x1000u, 64u}, CallRange{0x2000u, 32u}, CallRange{0x5000u, 1u}}}}}}, kLeaves1',
                      text)
        self.assertIn('}}, CallRecipes{}}, kLeaves0', text)
        self.assertNotIn('engineCalls', text)  # the leaves stay the ShroudForge profile's
        self.assertEqual(len(self.rows(text)), len(self.rows((ROOT / g.OUTPUT).read_text(encoding='utf-8'))))
        cxx = shutil.which('g++') or shutil.which('c++') or shutil.which('clang++')
        if not cxx:
            self.skipTest('no C++ compiler on PATH')
        tmp = Path(tempfile.mkdtemp(prefix='engine-calls-cxx-'))
        self.addCleanup(shutil.rmtree, tmp)
        (tmp / 'profiles.g.h').write_text(text)
        (tmp / 'check.cpp').write_text(
            '#include "profiles.g.h"\n'
            'using namespace kiln::engine;\n'
            'constexpr const CallRecipes& c = generated::kProfiles[1].layout.calls;\n'
            'static_assert(generated::kProfiles[1].layout.dedicated_server);\n'
            'static_assert(c.recipe[0].id == 0 && c.recipe[1].id == 0 && c.recipe[3].id == 0 && c.recipe[7].id == 0);\n'
            'static_assert(c.recipe[2].id == static_cast<std::uint32_t>(CallRecipeId::ImpactSameEntity));\n'
            'static_assert(c.recipe[2].arg_count == 2 && c.recipe[2].arg_bytes[0] == 64 && c.recipe[2].arg_bytes[7] == 0);\n'
            'static_assert(c.recipe[2].result_bits == 32 && c.recipe[2].range_count == 3);\n'
            'static_assert(c.recipe[2].ranges[1].rva == 0x2000 && c.recipe[2].ranges[1].size == 32);\n'
            'static_assert(c.recipe[2].ranges[2].rva == 0x5000 && c.recipe[2].ranges[3].rva == 0 && c.recipe[2].ranges[31].size == 0);\n'
            'static_assert(generated::kProfiles[0].layout.calls.recipe[2].id == 0);\n'
            'int main() { return 0; }\n')
        done = subprocess.run([cxx, '-std=c++20', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                               '-I' + str(ROOT / 'native/services/engine'), '-I' + str(ROOT / 'native/core/abi'),
                               '-I' + str(tmp), str(tmp / 'check.cpp')], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr[-3000:])


def fake_pe(size_of_image, code, va=0x1000, raw=0x400, extra=(), relocs=(), text_vsize=None):
    """A minimal AMD64 PE32+ file: a .text section at `va` whose file data (at `raw`) is `code`. `extra` adds
    (name, va, data) executable sections after it; `relocs` adds a .reloc section and base relocation directory of
    (rva, type) entries (IMAGE_REL_BASED_*: 10 DIR64, 3 HIGHLOW, 0 ABSOLUTE padding). `text_vsize`: .text's VirtualSize
    when it is not the raw size (the loader zero-fills the rest)."""
    import struct
    sections = [(b'.text', va, code, 0x60000020)] + [(name.encode(), sva, sdata, 0x60000020) for name, sva, sdata in extra]
    blocks = b''
    if relocs:
        pages = {}
        for rva, kind in relocs:
            pages.setdefault(rva & ~0xfff, []).append((kind << 12) | (rva & 0xfff))
        for page, entries in sorted(pages.items()):
            if len(entries) % 2:
                entries.append(0)
            blocks += struct.pack('<II', page, 8 + 2 * len(entries)) + b''.join(struct.pack('<H', e) for e in entries)
        _, last_va, last_data, _ = sections[-1]
        sections.append((b'.reloc', (last_va + len(last_data) + 0xfff) & ~0xfff, blocks, 0x42000040))
    data = bytearray(raw)
    data[0:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x40)
    data[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<HH', data, 0x44, 0x8664, len(sections))
    struct.pack_into('<H', data, 0x54, 0xf0)
    struct.pack_into('<H', data, 0x58, 0x20b)
    struct.pack_into('<I', data, 0x58 + 56, size_of_image)
    struct.pack_into('<I', data, 0x58 + 108, 16)
    if relocs:
        struct.pack_into('<II', data, 0x58 + 112 + 8 * 5, sections[-1][1], len(blocks))
    for i, (name, sva, sdata, flags) in enumerate(sections):
        s = 0x58 + 0xf0 + 40 * i
        data[s:s + 8] = name.ljust(8, b'\0')
        vsize = text_vsize if i == 0 and text_vsize is not None else len(sdata)
        struct.pack_into('<IIII', data, s + 8, vsize, sva, len(sdata), len(data))
        struct.pack_into('<I', data, s + 36, flags)
        data += sdata
    return bytes(data)


class EngineCallOriginals(unittest.TestCase):
    """--check: each recipe range's original_sha256 against a read-only copy of its exact executable, when there is one."""

    CODE = bytes((i * 37 + 11) & 0xff for i in range(0x800))

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='engine-originals-'))
        self.addCleanup(shutil.rmtree, self.tmp)

    def exe(self, size_of_image=31318016, name='enshrouded_server.exe', folder='server-x'):
        data = fake_pe(size_of_image, self.CODE)
        path = self.tmp / 'bin' / folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return hashlib.sha256(data).hexdigest()

    @classmethod
    def digest(cls, rva, size):
        return hashlib.sha256(cls.CODE[rva - 0x1000: rva - 0x1000 + size]).hexdigest()

    def profile(self, sha, ranges, size=31318016):
        recipe = {'name': 'PropSpawnSubmit', 'id': 1, 'arg_bytes': [], 'result_bits': 0, 'ranges': ranges}
        return {'path': Path('server/26430253.json'), 'target': 'enshrouded_server.exe', 'sha256': sha, 'size': size,
                'calls': [recipe] + [None] * 7}

    def test_matching_bytes(self):
        sha = self.exe()
        ranges = [(0x1000, 64, self.digest(0x1000, 64)), (0x1400, 0x400, self.digest(0x1400, 0x400))]
        checks = g.original_checks([self.profile(sha, ranges)], self.tmp / 'bin')
        self.assertEqual([ok for ok, _ in checks], [True, True], checks)

    def test_different_bytes_fail_the_range_that_differs(self):
        sha = self.exe()
        ranges = [(0x1000, 64, self.digest(0x1000, 64)), (0x1100, 16, self.digest(0x1101, 16))]
        checks = g.original_checks([self.profile(sha, ranges)], self.tmp / 'bin')
        self.assertEqual([ok for ok, _ in checks], [True, False], checks)
        self.assertIn('ranges.1 (rva 0x1100, 16 bytes): original bytes differ', checks[1][1])

    def test_a_range_outside_the_file_data(self):
        sha = self.exe()
        checks = g.original_checks([self.profile(sha, [(0x17f0, 0x20, '0' * 64), (0x800, 16, '0' * 64)])], self.tmp / 'bin')
        self.assertEqual([ok for ok, _ in checks], [False, False], checks)
        self.assertIn("not inside .text's file data", checks[0][1])

    def test_a_range_outside_text(self):
        # read_original reads only the exe's .text: a range in another executable section (the client's .bind) can
        # never pass the member's pre-gate check, so the build refuses it too
        bind = bytes((i * 11 + 5) & 0xff for i in range(0x200))
        data = fake_pe(31318016, self.CODE, extra=[('.bind', 0x2000, bind)])
        path = self.tmp / 'bin/server-x/enshrouded_server.exe'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        sha = hashlib.sha256(data).hexdigest()
        ranges = [(0x1000, 64, self.digest(0x1000, 64)), (0x2000, 16, hashlib.sha256(bind[:16]).hexdigest())]
        checks = g.original_checks([self.profile(sha, ranges)], self.tmp / 'bin')
        self.assertEqual([ok for ok, _ in checks], [True, False], checks)
        self.assertIn("ranges.1 (rva 0x2000, 16 bytes): not inside .text's file data", checks[1][1])

    def test_section_edges(self):
        # .text's VirtualSize beyond its raw data (a zero-filled tail, no file byte) and a range running from .text into
        # the next section: neither is wholly inside .text's file data
        tail = bytes(0x100)
        bind = bytes((i * 11 + 5) & 0xff for i in range(0x200))
        data = fake_pe(31318016, self.CODE, text_vsize=len(self.CODE) + len(tail), extra=[('.bind', 0x1900, bind)])
        path = self.tmp / 'bin/server-x/enshrouded_server.exe'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        sha = hashlib.sha256(data).hexdigest()
        ranges = [(0x17f0, 0x10, self.digest(0x17f0, 0x10)),                       # the last raw bytes: inside
                  (0x1800, 0x10, hashlib.sha256(tail[:0x10]).hexdigest()),         # the zero-filled tail
                  (0x17f8, 0x10, hashlib.sha256(self.CODE[-8:] + tail[:8]).hexdigest()),  # raw data into the tail
                  (0x18f8, 0x10, hashlib.sha256(tail[-8:] + bind[:8]).hexdigest())]       # .text into .bind
        checks = g.original_checks([self.profile(sha, ranges)], self.tmp / 'bin')
        self.assertEqual([ok for ok, _ in checks], [True, False, False, False], checks)

    def test_a_range_over_a_base_relocation(self):
        # read_original gives the file's bytes relocated to where the exe is loaded: a range holding a fixup field would
        # differ from its original_sha256 at run time whenever the exe is not at its preferred base
        data = fake_pe(31318016, self.CODE, relocs=[(0x1100, 10), (0x1200, 3), (0x1300, 0)])
        path = self.tmp / 'bin/server-x/enshrouded_server.exe'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        sha = hashlib.sha256(data).hexdigest()
        cases = [((0x1000, 0x100), True),   # ends right before the DIR64 field
                 ((0x10f8, 9), False),      # its first byte
                 ((0x1104, 4), False),      # inside it
                 ((0x1107, 2), False),      # its last byte
                 ((0x1108, 0xf8), True),    # right after it, up to the HIGHLOW field
                 ((0x1203, 1), False),      # the HIGHLOW field's last byte
                 ((0x1204, 4), True),       # after it
                 ((0x1300, 8), True)]       # ABSOLUTE padding is no fixup
        ranges = [(rva, size, self.digest(rva, size)) for (rva, size), _ in cases]
        checks = g.original_checks([self.profile(sha, ranges)], self.tmp / 'bin')
        self.assertEqual([ok for ok, _ in checks], [ok for _, ok in cases], checks)
        self.assertIn('ranges.1 (rva 0x10f8, 9 bytes): holds a base relocation (rva 0x1100, 8 bytes)', checks[1][1])
        self.assertIn('ranges.5 (rva 0x1203, 1 bytes): holds a base relocation (rva 0x1200, 4 bytes)', checks[5][1])

    def test_an_unknown_relocation_type_fails_closed(self):
        data = fake_pe(31318016, self.CODE, relocs=[(0x1100, 4)])
        path = self.tmp / 'bin/server-x/enshrouded_server.exe'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        with self.assertRaises(g.ProfileError) as caught:
            g.original_checks([self.profile(hashlib.sha256(data).hexdigest(), [(0x1000, 4, self.digest(0x1000, 4))])],
                              self.tmp / 'bin')
        self.assertIn('base relocation type 4', str(caught.exception))

    def test_only_the_exact_copy_is_used(self):
        self.exe()
        other = self.exe(size_of_image=0x20000, folder='server-y', name='enshrouded_server.exe.bak')  # its own sha256
        checks = g.original_checks([self.profile('f' * 64, [(0x1000, 4, self.digest(0x1000, 4))])], self.tmp / 'bin')
        self.assertEqual([ok for ok, _ in checks], [None], checks)
        self.assertIn('no copy of enshrouded_server.exe', checks[0][1])
        checks = g.original_checks([self.profile(other, [(0x1000, 4, self.digest(0x1000, 4))])], self.tmp / 'bin')
        self.assertEqual([ok for ok, _ in checks], [None], checks)  # the right bytes under another name: not the target

    def test_a_copy_with_another_size_of_image(self):
        sha = self.exe(size_of_image=0x10000)
        checks = g.original_checks([self.profile(sha, [(0x1000, 4, self.digest(0x1000, 4))])], self.tmp / 'bin')
        self.assertEqual([ok for ok, _ in checks], [False], checks)
        self.assertIn("SizeOfImage 65536 is not the profile's 31318016", checks[0][1])

    def test_profiles_without_recipes_are_not_looked_up(self):
        self.assertEqual(g.original_checks([dict(self.profile('0' * 64, []), calls=[None] * 8)], self.tmp / 'nothing'), [])

    def test_check_command_line(self):
        sha = self.exe()
        root = self.tmp / 'root'
        for path in sorted((ROOT / 'sigdb/engine').glob('*/*.json')):
            p = json.loads(path.read_text())
            if path.parent.name == 'server':
                p['image']['sha256'] = sha
                p['engineCalls'] = {'PropSpawnSubmit': EngineCalls.recipe(1, ((0x1000, 64), (0x1200, 8)))}
                for r in p['engineCalls']['PropSpawnSubmit']['ranges']:
                    r['original_sha256'] = self.digest(r['rva'], r['size'])
            out = root / 'sigdb/engine' / path.parent.name / path.name
            out.parent.mkdir(parents=True)
            out.write_text(json.dumps(p, indent=2))
        script = str(Path(g.__file__))
        env = {k: v for k, v in os.environ.items() if k != 'KILN_ENGINE_BINARIES'}
        made = subprocess.run([sys.executable, '-I', script, '--root', str(root)], capture_output=True, text=True, env=env)
        self.assertEqual(made.returncode, 0, made.stderr)
        done = subprocess.run([sys.executable, '-I', script, '--root', str(root), '--check', '--binaries',
                               str(self.tmp / 'bin')], capture_output=True, text=True, env=env)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn('engine call originals: 2 checked, 0 failed, 0 profiles NOT CHECKED (no copy of their build)', done.stdout)
        done = subprocess.run([sys.executable, '-I', script, '--root', str(root), '--check'], capture_output=True,
                              text=True, env=env)
        self.assertEqual(done.returncode, 0, done.stderr)
        # no copies named: the recipes' originals are said to be unchecked, never left out (nor passed off as checked)
        self.assertIn('engine call originals: NOT CHECKED, no --binaries or KILN_ENGINE_BINARIES (2 ranges in 1 profiles)',
                      done.stdout)
        self.assertNotIn('ok   ', done.stdout)
        done = subprocess.run([sys.executable, '-I', script, '--root', str(root), '--check', '--binaries',
                               str(self.tmp / 'nothing-here')], capture_output=True, text=True, env=env)
        self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
        (self.tmp / 'empty').mkdir()
        done = subprocess.run([sys.executable, '-I', script, '--root', str(root), '--check', '--binaries',
                               str(self.tmp / 'empty')], capture_output=True, text=True, env=env)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn('NOT CHECKED 26430253.json: no copy of enshrouded_server.exe', done.stdout)
        self.assertIn('engine call originals: 0 checked, 0 failed, 1 profiles NOT CHECKED (no copy of their build)', done.stdout)
        data = bytearray((self.tmp / 'bin/server-x/enshrouded_server.exe').read_bytes())
        server = root / 'sigdb/engine/server/26430253.json'
        p = json.loads(server.read_text())
        p['engineCalls']['PropSpawnSubmit']['ranges'][1]['original_sha256'] = self.digest(0x1201, 8)
        server.write_text(json.dumps(p, indent=2))
        subprocess.run([sys.executable, '-I', script, '--root', str(root)], capture_output=True, text=True, env=env, check=True)
        done = subprocess.run([sys.executable, '-I', script, '--root', str(root), '--check'], capture_output=True,
                              text=True, env=dict(env, KILN_ENGINE_BINARIES=str(self.tmp / 'bin')))
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn('FAIL 26430253.json PropSpawnSubmit ranges.1 (rva 0x1200, 8 bytes): original bytes differ', done.stdout)
        self.assertEqual(bytes(data), (self.tmp / 'bin/server-x/enshrouded_server.exe').read_bytes())  # read only


@unittest.skipUnless(os.environ.get('KILN_ENGINE_BINARIES'), 'KILN_ENGINE_BINARIES not set (read-only exe copies)')
class EngineCallOriginalsRealCopies(unittest.TestCase):
    """Against the real exact builds: a range over the change tracker's 33 guard bytes, whose original bytes each profile
    already pins (ecsWriteLayout.changeTrackerBytes), matches; one wrong digest fails."""

    def test_both_exact_builds(self):
        base = Path(os.environ['KILN_ENGINE_BINARIES'])
        loaded = [g.load(p) for p in sorted((ROOT / 'sigdb/engine').glob('*/*.json'))]
        for p in loaded:
            w = p['profile']['ecsWriteLayout']
            digest = hashlib.sha256(bytes.fromhex(w['changeTrackerBytes'])).hexdigest()
            p['calls'] = [{'name': 'PropSpawnSubmit', 'id': 1, 'arg_bytes': [], 'result_bits': 0,
                           'ranges': [(w['changeTrackerRva'], 33, digest)]}] + [None] * 7
        checks = g.original_checks(loaded, base)
        self.assertEqual([ok for ok, _ in checks], [True, True], checks)
        loaded[0]['calls'][0]['ranges'] = [(loaded[0]['calls'][0]['ranges'][0][0] + 1, 33, loaded[0]['calls'][0]['ranges'][0][2])]
        self.assertEqual([ok for ok, _ in g.original_checks(loaded, base)], [False, True])
        # the client's executable .bind section is not .text: read_original refuses it, so the check does too
        client = loaded[0]
        self.assertEqual(client['target'], 'enshrouded.exe')
        copy = next(p for p in sorted(base.rglob('enshrouded.exe')) if hashlib.sha256(p.read_bytes()).hexdigest() == client['sha256'])
        _, sections = g.pe_sections(copy.read_bytes())
        bind = [s for s in sections if s[0] == '.bind']
        self.assertEqual(len(bind), 1, sections)
        _, va, _, raw, _ = bind[0]
        digest = hashlib.sha256(copy.read_bytes()[raw:raw + 16]).hexdigest()
        client['calls'][0]['ranges'] = [(va, 16, digest)]
        checks = g.original_checks(loaded[:1], base)
        self.assertEqual([ok for ok, _ in checks], [False], checks)
        self.assertIn("not inside .text's file data", checks[0][1])


if __name__ == '__main__':
    unittest.main(verbosity=1)
