#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exact-build engine profiles -> native/services/engine/profiles.g.h (kiln.engine).

Input: sigdb/engine/<side>/<steam build>.json, ShroudForge's per-build profiles copied verbatim (MIT data, Copyright (c)
2026 bonsaibauer; sigdb/engine/README.md). Every rule ShroudForge applies before its engine context is configured runs
here, at build time, on that immutable data, so a profile that ShroudForge would refuse never ships:

- eligibility: Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer),
  modules/game-runtime/native/loaded-resources/select_resource_profile.cpp:5-13 (read_profile);
- resource layout: parse_resource_layout.cpp:4-110 (json_size, parse_layout), typed as kiln::engine::ResourceLayout;
- engine layout: modules/game-runtime/native/engine-context/configure_engine_layout.cpp:6-287 (Configure).

Every image_size bound uses the profile's own image.size: Configure refuses unless the loaded SizeOfImage equals it
(configure_engine_layout.cpp:17-21), so the result is the same as ShroudForge's at run time. The run-time identity
check (sha256, TimeDateStamp, SizeOfImage, target, KFC format) stays in the service (identity.cpp).

Python 3 stdlib only. --check regenerates in memory and compares.
"""
import argparse
import bisect
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = Path('native/services/engine/profiles.g.h')
SOURCE_PIN = 'bonsaibauer/shroudforge 38ef448dc5d4fc267b5f0928502a864a6b078865 (1.8.0), identical in fork d8a394bc (1.7.9)'
SECTION_BITS = {
    'owners': 1 << 0, 'ecs_read': 1 << 1, 'ecs_authority': 1 << 2, 'ecs_write': 1 << 3, 'resource_read': 1 << 4,
    'resource_write': 1 << 5, 'services': 1 << 6, 'queue': 1 << 7, 'spawn': 1 << 8, 'system_dependencies': 1 << 9,
    'item_id_map': 1 << 10,
}


# Kiln's own engineCalls section (not ShroudForge data; kiln_engine_layout.h CallRecipe): the
# reviewed engine calls of this exact build, keyed by the frozen CallRecipeId name. Each recipe lands with the service
# that calls it and that change's review (docs/kiln/services/engine-call-recipes.md). The names and values are
# kiln_engine_layout.h's enum CallRecipeId (test_generate.py compares them).
CALL_RECIPE_IDS = {'PropSpawnSubmit': 1, 'PropDestroySubmit': 2, 'ImpactSameEntity': 3}
CALL_RECIPES, CALL_RANGES, CALL_ARGUMENTS = 8, 32, 8  # kCallRecipes, kCallRanges, kCallArguments
CALL_CODE_BYTES = 64 * 1024  # kCallCodeBytes: one range and all of a recipe's ranges together, at most
CALL_RESULT_BITS = (0, 8, 16, 32, 64)
CALL_KEYS = ('id', 'argBytes', 'resultBits', 'ranges', 'review')
CALL_RANGE_KEYS = ('rva', 'size', 'original_sha256')
CALL_REVIEW_KEYS = ('sfSource', 'sfInHold', 'locks', 'allocators', 'threadState', 'stackBuffersMovedToHeap',
                    'boundedLoops')
CALL_THREAD_STATE_KEYS = ('mxcsr', 'engineTls')


class ProfileError(Exception):
    pass


def is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def get_size(obj, key):
    """nlohmann at(key).get<std::size_t>(): missing key or non-number throws; negative values wrap."""
    if not isinstance(obj, dict) or key not in obj:
        raise ProfileError('[json.exception.out_of_range.403] key \'%s\' not found' % key)
    value = obj[key]
    if isinstance(value, float):
        value = int(value)
    if not is_int(value):
        raise ProfileError('[json.exception.type_error.302] type must be number: ' + key)
    return value % (1 << 64)


def get_u32(obj, key):
    """get<std::uint32_t>() narrows like a C++ conversion."""
    return get_size(obj, key) % (1 << 32)


def get_unsigned(obj, key):
    return get_u32(obj, key)


def value_bool(obj, key, default=False):
    """nlohmann value(key, false): default when absent, type error when present and not boolean."""
    if not isinstance(obj, dict):
        raise ProfileError('[json.exception.type_error.306] cannot use value() with ' + type(obj).__name__)
    if key not in obj:
        return default
    if not isinstance(obj[key], bool):
        raise ProfileError('[json.exception.type_error.302] type must be boolean: ' + key)
    return obj[key]


def value_text(obj, key, default=''):
    if not isinstance(obj, dict):
        raise ProfileError('[json.exception.type_error.306] cannot use value() with ' + type(obj).__name__)
    if key not in obj:
        return default
    if not isinstance(obj[key], str):
        raise ProfileError('[json.exception.type_error.302] type must be string: ' + key)
    return obj[key]


def at(obj, key):
    if not isinstance(obj, dict) or key not in obj:
        raise ProfileError('[json.exception.out_of_range.403] key \'%s\' not found' % key)
    return obj[key]


def text(obj, key):
    value = at(obj, key)
    if not isinstance(value, str):
        raise ProfileError('[json.exception.type_error.302] type must be string: ' + key)
    return value


def eligible(profile, target):
    # select_resource_profile.cpp:5-13 (read_profile); any exception -> not eligible
    try:
        version = profile.get('schemaVersion', 0)
        provenance = profile.get('provenance')
        return (version in (1, 2) and not isinstance(version, bool) and profile.get('target') == target and
                profile.get('kfcFormat') == 4 and isinstance(provenance, dict) and
                provenance.get('status') == 'read-validated' and value_bool(provenance, 'resourceReadValidated'))
    except ProfileError:
        return False


# kiln::engine::ResourceLayout (kiln_engine_layout.h) in its field order: (C++ field, path in resourceLayout). ShroudForge's
# resource Layout (loaded_resource_state.h:48-65) without profile_id and dedicated_server.
RESOURCE_FIELDS = [
    ('main_active_context_offset', ('mainActiveContextOffset',)),
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
    ('maximum_resource_bytes', ('maximumResourceBytes',)),
]
# kiln::engine::ItemMapLayout after its `enabled` (loaded_resource_state.h:40-46): (C++ field, key in itemIdMap).
ITEM_MAP_FIELDS = [
    ('type_hash', 'targetHash'), ('part_index', 'partIndex'), ('root_size', 'targetSize'),
    ('item_id_offset', 'itemIdOffset'), ('guid_offset', 'guidOffset'), ('map_offset', 'mapOffset'),
    ('header_size', 'headerSize'), ('bits_offset', 'bitsPointerOffset'), ('capacity_offset', 'capacityOffset'),
    ('keys_offset', 'keysPointerOffset'), ('values_offset', 'valuesPointerOffset'), ('count_offset', 'countOffset'),
    ('maximum_capacity', 'maximumCapacity'), ('maximum_probes', 'maximumProbes'),
]


def parse_resource_layout(candidate, image_size):
    """parse_resource_layout.cpp:4-110 (json_size, parse_layout). Returns ShroudForge's resource Layout as 'typed'
    (RESOURCE_FIELDS, plus 'item_map' with 'enabled'), the reader offset and whether the item map is present."""
    def json_size(obj, key, maximum):
        result = get_size(obj, key)
        if result > maximum:
            raise ProfileError('out-of-range resource layout field: ' + key)
        return result
    value = at(candidate, 'resourceLayout')
    text(candidate, 'id')
    r = {}
    for key in ('mainActiveContextOffset', 'contextItemRegistryOffset', 'itemRegistryServiceOffset',
                'itemRegistryHandleOffset', 'itemRegistryResourcePointerOffset', 'resourceServiceReaderOffset',
                'resourceServiceIdCallbackOffset'):
        r[key] = json_size(value, key, 0x1000000)
    r['resourceServiceIdCallbackRva'] = json_size(value, 'resourceServiceIdCallbackRva', image_size)
    r['resourceServiceHandleCallbackOffset'] = json_size(value, 'resourceServiceHandleCallbackOffset', 0x1000000)
    r['resourceServiceHandleCallbackRva'] = json_size(value, 'resourceServiceHandleCallbackRva', image_size)
    for key in ('resourceServiceChunkTableOffset', 'resourceServiceChunkCountOffset', 'readerMetadataOffset',
                'metadataMapOffset'):
        r[key] = json_size(value, key, 0x1000000)
    m = at(value, 'map')
    mp = {k: json_size(m, k, 0x1000) for k in ('bucketRel32Offset', 'bucketCountOffset', 'keysRel32Offset',
                                                'keyCountOffset', 'valuesRel32Offset', 'valueCountOffset',
                                                'headerSize')}
    for k in ('bucketStride', 'keyStride', 'resourceKeySize', 'valueStride'):
        mp[k] = json_size(m, k, 64)
    if mp['bucketStride'] != 8 or mp['keyStride'] != 32 or mp['resourceKeySize'] != 24 or mp['valueStride'] != 8:
        raise ProfileError('unsupported ResourceService map record layout')
    e = at(value, 'valueEntry')
    resource_offset = json_size(e, 'resourceOffsetOffset', 7)
    packed = json_size(e, 'packedSizeOffset', 7)
    chunk_index = json_size(e, 'chunkIndexOffset', 7)
    size_mask = json_size(e, 'sizeMask', 0xffffff)
    if (resource_offset + 4 > mp['valueStride'] or packed + 4 > mp['valueStride'] or
            chunk_index >= mp['valueStride'] or size_mask != 0xffffff):
        raise ProfileError('invalid ResourceService value record layout')
    c = at(value, 'chunkEntry')
    stride = json_size(c, 'stride', 64)
    base = json_size(c, 'basePointerOffset', 64)
    size = json_size(c, 'sizeOffset', 64)
    if stride != 16 or base != 0 or size != 8:
        raise ProfileError('unsupported ResourceService chunk layout')
    maximum_entries = json_size(value, 'maximumEntries', 1 << 20)
    maximum_chunks = json_size(value, 'maximumChunks', 1 << 16)
    maximum_bytes = json_size(value, 'maximumResourceBytes', 256 << 20)
    if (not r['resourceServiceIdCallbackRva'] or r['resourceServiceIdCallbackRva'] >= image_size or
            not r['resourceServiceHandleCallbackRva'] or r['resourceServiceHandleCallbackRva'] >= image_size or
            mp['bucketCountOffset'] + 4 > mp['headerSize'] or mp['valueCountOffset'] + 4 > mp['headerSize'] or
            mp['keyCountOffset'] + 4 > mp['headerSize'] or mp['bucketRel32Offset'] + 4 > mp['headerSize'] or
            mp['keysRel32Offset'] + 4 > mp['headerSize'] or mp['valuesRel32Offset'] + 4 > mp['headerSize'] or
            not maximum_entries or not maximum_chunks or not maximum_bytes):
        raise ProfileError('incomplete ResourceService profile layout')
    item_map = False
    item = {'enabled': False, **{field: 0 for field, _ in ITEM_MAP_FIELDS}}
    if 'itemIdMap' in value:
        model = at(value, 'itemIdMap')
        provenance = at(candidate, 'provenance')
        source = at(model, 'source')
        evidence = at(provenance, 'itemIdMapEvidence')
        if (at(model, 'id') != 'original-item-id-map-v1' or at(model, 'registry') != 'g38_items::ItemRegistry' or
                at(model, 'targetType') != 'keen::ItemInfo' or not value_bool(provenance, 'itemIdMapValidated') or
                not isinstance(source, str) or not source or not isinstance(evidence, str) or not evidence):
            raise ProfileError('unproven original ItemRegistry map contract')
        type_hash = get_u32(model, 'targetHash')
        part_index = get_u32(model, 'partIndex')
        root_size = json_size(model, 'targetSize', maximum_bytes)
        item_id = json_size(model, 'itemIdOffset', maximum_bytes)
        guid = json_size(model, 'guidOffset', maximum_bytes)
        map_offset = json_size(model, 'mapOffset', 0x1000000)
        header = json_size(model, 'headerSize', 128)
        bits = json_size(model, 'bitsPointerOffset', 128)
        capacity = json_size(model, 'capacityOffset', 128)
        keys = json_size(model, 'keysPointerOffset', 128)
        values = json_size(model, 'valuesPointerOffset', 128)
        count = json_size(model, 'countOffset', 128)
        maximum_capacity = json_size(model, 'maximumCapacity', 1 << 20)
        maximum_probes = json_size(model, 'maximumProbes', 256)
        wrap = lambda v: v % (1 << 64)
        if (not type_hash or root_size < 20 or item_id > wrap(root_size - 4) or guid > wrap(root_size - 16) or
                item_id % 4 or header < 8 or bits > wrap(header - 8) or capacity > wrap(header - 8) or
                keys > wrap(header - 8) or values > wrap(header - 8) or count > wrap(header - 4) or
                not maximum_capacity or not maximum_probes or part_index != 0):
            raise ProfileError('invalid original ItemRegistry map contract')
        item_map = True
        item = {'enabled': True, 'type_hash': type_hash, 'part_index': part_index, 'root_size': root_size,
                'item_id_offset': item_id, 'guid_offset': guid, 'map_offset': map_offset, 'header_size': header,
                'bits_offset': bits, 'capacity_offset': capacity, 'keys_offset': keys, 'values_offset': values,
                'count_offset': count, 'maximum_capacity': maximum_capacity, 'maximum_probes': maximum_probes}
    validated = dict(r)
    validated.update({'map.' + k: v for k, v in mp.items()})
    validated.update({'valueEntry.resourceOffsetOffset': resource_offset, 'valueEntry.packedSizeOffset': packed,
                      'valueEntry.chunkIndexOffset': chunk_index, 'valueEntry.sizeMask': size_mask,
                      'chunkEntry.stride': stride, 'chunkEntry.basePointerOffset': base, 'chunkEntry.sizeOffset': size,
                      'maximumEntries': maximum_entries, 'maximumChunks': maximum_chunks,
                      'maximumResourceBytes': maximum_bytes})
    typed = {field: validated['.'.join(path)] for field, path in RESOURCE_FIELDS}
    typed['item_map'] = item
    return {'resource_service_reader_offset': r['resourceServiceReaderOffset'], 'item_id_map': item_map,
            'typed': typed}


def configure(profile, image_size, target):
    """configure_engine_layout.cpp:6-287 (Configure), returning the engine Layout ShroudForge stores."""
    version = at(profile, 'schemaVersion')
    if not is_int(version) or version not in (1, 2):
        raise ProfileError('unsupported engine owner profile format')
    if version == 1 and ('entityCommandQueueLayout' in profile or 'buildingPropSpawnLayout' in profile):
        raise ProfileError('explicit queue and spawn layouts require profile format version 2')
    identity = at(profile, 'image')
    if at(profile, 'target') != target or not isinstance(at(identity, 'sha256'), str):
        raise ProfileError('engine owner profile does not match the exact loaded image')
    provenance = at(profile, 'provenance')
    owners = at(profile, 'ownerLayout')
    lay = {'image_size': image_size, 'dedicated_server': target == 'enshrouded_server.exe'}

    def offset(obj, key, maximum=0x4000000):
        value = get_size(obj, key)
        if value > maximum:
            raise ProfileError('engine owner offset out of range: ' + key)
        return value
    lay['root_rva'] = offset(owners, 'rootObjectRva', image_size - 1)
    lay['root_vtable'] = offset(owners, 'rootVtableRva', image_size - 1)
    lay['client_offset'] = offset(owners, 'clientPointerOffset')
    lay['client_vtable'] = offset(owners, 'clientVtableRva', image_size - 1)
    lay['controller_offset'] = offset(owners, 'controllerPointerOffset')
    lay['controller_vtable'] = offset(owners, 'controllerVtableRva', image_size - 1)
    lay['controller_back_pointer'] = offset(owners, 'controllerClientBackPointerOffset')
    lay['main_owner_offset'] = offset(owners, 'controllerMainOwnerOffset')
    sections = SECTION_BITS['owners']
    if 'systemDependencyLayout' in profile:
        if not value_bool(provenance, 'systemDependenciesValidated'):
            raise ProfileError('system dependencies lack original scheduler validation')
        model = at(profile, 'systemDependencyLayout')
        slots = at(model, 'slots')
        source = text(model, 'source')
        if at(model, 'id') != 'ecs-system-dependencies-v1' or not source or not isinstance(slots, list) or len(slots) != 6:
            raise ProfileError('invalid system dependency model')
        seen = 0
        for slot in slots:
            position = get_unsigned(slot, 'offset')
            role = text(slot, 'storageRole')
            access = text(slot, 'accessMode')
            index = (position - 24) // 16 if position >= 24 else 0
            if (position < 24 or position > 104 or (position - 24) % 16 or (seen & (1 << index)) or
                    role not in ('runtime', 'template') or
                    (access != 'configuration-read' if role == 'template' else access not in ('read', 'write'))):
                raise ProfileError('invalid or duplicate system dependency slot')
            seen |= 1 << index
            offset(slot, 'systemRecordMaskOffset', 0x10000)
        offset(model, 'recordBuilderRva', image_size - 1)
        offset(model, 'accessSetPackRva', image_size - 1)
        offset(model, 'compatibilityPredicateRva', image_size - 1)
        sections |= SECTION_BITS['system_dependencies']
    ent = {'manager_occupancy': 0, 'manager_keys': 0, 'entity_definition_uuid': 0, 'entity_definition_uuid_validated': False}
    lay.update({'execution_owner_offset': 0, 'registry_owner_offset': 0, 'entity_manager_offset': 0,
                'manager_owner_offset': 0, 'authority': False, 'ecs': False, 'client_scene_offset': 0,
                'scene_entries_offset': 0, 'scene_count_offset': 0, 'scene_capacity_offset': 0, 'entry_stride': 0,
                'entry_context_offset': 0, 'entry_uuid_offset': 0, 'entry_state_offset': 0, 'maximum_scenes': 0,
                'entry_ready_state': 0})
    if 'ecsReadLayout' in profile:
        if not value_bool(provenance, 'ecsReadValidated'):
            raise ProfileError('ECS owner layout lacks read validation')
        ecs = at(profile, 'ecsReadLayout')
        lay['execution_owner_offset'] = offset(ecs, 'mainExecutionOwnerOffset')
        lay['registry_owner_offset'] = offset(ecs, 'executionRegistryOwnerOffset')
        lay['entity_manager_offset'] = offset(ecs, 'executionEntityManagerOffset')
        lay['manager_owner_offset'] = offset(ecs, 'entityManagerOwnerOffset')
        ent['manager_count'] = offset(ecs, 'entityManagerCount')
        ent['manager_table'] = offset(ecs, 'entityManagerTable')
        if ('entityManagerOccupancy' in ecs) != ('entityManagerKeys' in ecs):
            raise ProfileError('ECS entity hash membership needs both occupancy and keys offsets')
        if 'entityManagerOccupancy' in ecs:
            ent['manager_occupancy'] = offset(ecs, 'entityManagerOccupancy')
            ent['manager_keys'] = offset(ecs, 'entityManagerKeys')
            if not ent['manager_occupancy'] or not ent['manager_keys']:
                raise ProfileError('ECS entity hash membership offsets are zero')
        for key, name in (('entity_id', 'entityId'), ('entity_generation', 'entityGeneration'),
                          ('entity_layout', 'entityLayout'), ('entity_storage', 'entityStorage'),
                          ('entity_definition', 'entityDefinitionPointer'), ('entity_row', 'entityRow')):
            ent[key] = offset(ecs, name)
        ent['entity_definition_uuid_validated'] = 'entityDefinitionUuidOffset' in ecs
        if ent['entity_definition_uuid_validated']:
            ent['entity_definition_uuid'] = offset(ecs, 'entityDefinitionUuidOffset', 65536)
        ent['component_bits'] = offset(ecs, 'componentBits')
        ent['component_offsets'] = offset(ecs, 'componentOffsets')
        ent['component_strides'] = offset(ecs, 'componentStrides')
        ent['maximum_entities'] = offset(ecs, 'maximumEntities', 1 << 20)
        if not ent['maximum_entities']:
            raise ProfileError('engine entity limit is zero')
        if 'clientSceneOwnerOffset' in ecs:
            if not value_bool(provenance, 'ecsAuthorityReadValidated'):
                raise ProfileError('local authority owner layout lacks read validation')
            lay['client_scene_offset'] = offset(ecs, 'clientSceneOwnerOffset')
            lay['scene_entries_offset'] = offset(ecs, 'sceneEntriesPointerOffset')
            lay['scene_count_offset'] = offset(ecs, 'sceneEntriesCountOffset')
            lay['scene_capacity_offset'] = offset(ecs, 'sceneEntriesCapacityOffset')
            lay['entry_stride'] = offset(ecs, 'sceneEntryStride', 0x10000)
            lay['entry_context_offset'] = offset(ecs, 'sceneEntryContextOffset')
            lay['entry_uuid_offset'] = offset(ecs, 'sceneEntryUuidOffset')
            lay['entry_state_offset'] = offset(ecs, 'sceneEntryStateOffset')
            lay['entry_ready_state'] = offset(ecs, 'sceneEntryReadyState', 255) & 0xff
            lay['maximum_scenes'] = offset(ecs, 'maximumSceneEntries', 4096)
            s = lay['entry_stride']
            if (not lay['maximum_scenes'] or s < 24 or lay['entry_context_offset'] > s - 8 or
                    lay['entry_uuid_offset'] > s - 16 or lay['entry_state_offset'] >= s):
                raise ProfileError('invalid active scene entry layout')
            lay['authority'] = True
            sections |= SECTION_BITS['ecs_authority']
        lay['ecs'] = True
        sections |= SECTION_BITS['ecs_read']
    for k in ('manager_count', 'manager_table', 'entity_id', 'entity_generation', 'entity_layout', 'entity_storage',
              'entity_definition', 'entity_row', 'component_bits', 'component_offsets', 'component_strides',
              'maximum_entities'):
        ent.setdefault(k, 0)
    lay['entities'] = ent
    w = {'enabled': False}
    if 'ecsWriteLayout' in profile:
        model = at(profile, 'ecsWriteLayout')
        if not lay['authority'] or at(model, 'id') != 'ecs-native-thread-scene-gate-v1' or \
                not value_bool(provenance, 'ecsWriteGateValidated'):
            raise ProfileError('ECS write gate lacks original authority/lifetime validation')
        w['enabled'] = value_bool(model, 'enabled')
        if w['enabled'] and not value_bool(provenance, 'ecsChangeTrackingValidated'):
            raise ProfileError('ECS writes lack change tracking validation')
        if w['enabled'] and not value_bool(model, 'phaseValidated'):
            raise ProfileError('ECS writes lack original command/epoch phase validation')
        if value_text(model, 'phasePolicy') != 'untracked-components':
            raise ProfileError('ECS write phase policy must be untracked-components')
        w['registry_lock'] = offset(model, 'threadRegistryLockRva', image_size - 4)
        w['registry_entries'] = offset(model, 'threadRegistryEntriesRva', image_size - 1)
        w['registry_count'] = offset(model, 'threadRegistryCountRva', image_size - 2)
        w['callback'] = offset(model, 'threadCallbackRva', image_size - 1)
        w['capacity'] = offset(model, 'threadRegistryCapacity', 256)
        w['stride'] = offset(model, 'threadRegistryEntryStride', 256)
        w['row_handle'] = offset(model, 'registryEntryHandleOffset', 256)
        w['row_name'] = offset(model, 'registryEntryNamePointerOffset', 256)
        w['row_length'] = offset(model, 'registryEntryNameLengthOffset', 256)
        for key, name in (('inline_name', 'threadInlineNameOffset'), ('scene_thread', 'sceneThreadOffset'),
                          ('name_pointer', 'threadNamePointerOffset'), ('name_length', 'threadNameLengthOffset'),
                          ('name_capacity', 'threadNameCapacityOffset')):
            w[key] = offset(model, name, 65536)
        w['name_capacity_value'] = offset(model, 'threadNameCapacity', 64)
        for key, name in (('thread_handle', 'threadHandleOffset'), ('thread_context', 'threadContextOffset'),
                          ('thread_stop', 'threadStopOffset'), ('thread_callback', 'threadCallbackOffset'),
                          ('scene_context', 'sceneContextOffset'), ('scene_lock', 'sceneLockOffset'),
                          ('scene_state', 'sceneStateOffset')):
            w[key] = offset(model, name, 65536)
        w['running_state'] = offset(model, 'sceneRunningState', 255) & 0xff
        w['execution_mode'] = offset(model, 'executionModeOffset')
        w['authority_mode'] = offset(model, 'executionAuthorityMode', 255) & 0xff
        w['maximum_bytes'] = offset(model, 'maximumWriteBytes', 65536)
        w['tracker'] = offset(model, 'changeTrackerRva', image_size - 33)
        w['tracking_context'] = offset(model, 'changeTrackerContextOffset')
        for key, name in (('tracking_manager', 'changeTrackerManagerOffset'),
                          ('tracking_queue_b', 'changeTrackerQueueBOffset'),
                          ('tracking_registry_a', 'changeTrackerRegistryAOffset'),
                          ('tracking_registry_b', 'changeTrackerRegistryBOffset')):
            w[key] = offset(model, name, 65536)
        w['maximum_tracking_capacity'] = offset(model, 'maximumTrackingQueueCapacity', 1 << 20)
        tracker_bytes = text(model, 'changeTrackerBytes')
        if (len(tracker_bytes) != 66 or not w['tracker'] or not w['maximum_tracking_capacity'] or
                not w['tracking_registry_a'] or not w['tracking_registry_b'] or
                w['tracking_registry_a'] == w['tracking_registry_b']):
            raise ProfileError('invalid original ECS change tracker')
        if any(ch not in '0123456789abcdef' for ch in tracker_bytes):
            raise ProfileError('invalid change tracker guard bytes')
        w['tracker_bytes'] = bytes.fromhex(tracker_bytes)
        name = text(model, 'threadName')
        if (not w['capacity'] or w['stride'] < 24 or not w['maximum_bytes'] or not w['registry_lock'] or
                w['registry_lock'] % 4 or w['capacity'] * w['stride'] > image_size or
                w['registry_entries'] > image_size - w['capacity'] * w['stride'] or
                w['row_handle'] > w['stride'] - 8 or w['row_name'] > w['stride'] - 8 or w['row_length'] > w['stride'] - 8 or
                not name or len(name.encode()) > w['name_capacity_value'] or '\0' in name or
                w['name_capacity_value'] > 64 or not w['inline_name']):
            raise ProfileError('invalid native-thread ECS write gate layout')
        w['name'] = name.encode()
        if w['enabled']:
            sections |= SECTION_BITS['ecs_write']
    lay['write'] = w
    lay['queue'] = None
    lay['spawn'] = None
    if version == 2 and lay['authority'] and w['enabled'] and \
            ('entityCommandQueueLayout' not in profile or 'buildingPropSpawnLayout' not in profile):
        raise ProfileError('version 2 profile requires explicit queue and spawn layouts')
    if version == 1 and lay['authority'] and w['enabled'] and profile.get('kfcFormat', 0) == 4:
        lay['queue'] = {'task_state': 0x538, 'task_records': 0x528, 'task_count': 0x530, 'queue_records': 0xeb9f08,
                        'queue_count': 0xeb9f10, 'task_record_stride': 0xe0, 'entity_id_counter': 0x498,
                        'active_tracking_contexts': 0xebbfc0, 'shared_tracker': 0xebc030, 'manager_owner': 0,
                        'execution_mode': lay['entity_manager_offset'] + 8, 'buffer_capacity': 0x100000,
                        'authority_mode': 0}
        lay['spawn'] = {'source_entity_ids': 2088, 'allocation_failure_marker': 0}
    if version == 2 and 'entityCommandQueueLayout' in profile:
        model = at(profile, 'entityCommandQueueLayout')
        if (not lay['ecs'] or not lay['authority'] or not w['enabled'] or not isinstance(model, dict) or
                len(model) != 10 or at(model, 'id') != 'ecs-entity-command-queue-v1'):
            raise ProfileError('invalid original entity command queue layout')

        def number(key, maximum=0x4000000):
            value = at(model, key)
            if not is_int(value) or value < 0 or not value or value > maximum:
                raise ProfileError('invalid entity command queue value: ' + key)
            return value
        q = {'task_state': number('taskStateOffset'), 'task_records': number('taskRecordsOffset'),
             'task_count': number('taskCountOffset'), 'queue_records': number('queueRecordsOffset'),
             'queue_count': number('queueCountOffset'), 'task_record_stride': number('taskRecordStride', 0x10000),
             'entity_id_counter': number('entityIdCounterOffset'),
             'active_tracking_contexts': number('activeTrackingContextsOffset'),
             'buffer_capacity': number('bufferCapacity', 0xffffffff)}
        if q['task_record_stride'] < 24 or q['buffer_capacity'] < 160:
            raise ProfileError('invalid original entity command queue extent')
        q['shared_tracker'] = w['tracking_context']
        q['manager_owner'] = lay['manager_owner_offset']
        q['execution_mode'] = w['execution_mode']
        q['authority_mode'] = w['authority_mode']
        lay['queue'] = q
    if version == 2 and 'buildingPropSpawnLayout' in profile:
        model = at(profile, 'buildingPropSpawnLayout')
        if lay['queue'] is None or not isinstance(model, dict) or len(model) != 3 or \
                at(model, 'id') != 'building-prop-spawn-v1':
            raise ProfileError('invalid original building prop spawn layout')

        def ranged(key, minimum, maximum):
            value = at(model, key)
            if not is_int(value) or value < 0 or value < minimum or value > maximum:
                raise ProfileError('invalid building prop spawn value: ' + key)
            return value
        lay['spawn'] = {'source_entity_ids': ranged('sourceEntityIdsOffset', 1, 0x4000000),
                        'allocation_failure_marker': ranged('allocationFailureMarkerValue', 0, 0xffffffff)}
    if lay['queue'] is not None:
        sections |= SECTION_BITS['queue']
    if lay['spawn'] is not None:
        sections |= SECTION_BITS['spawn']
    r = {'enabled': False}
    if 'resourceWriteLayout' in profile:
        model = at(profile, 'resourceWriteLayout')
        if at(model, 'id') != 'resource-atomic-scalar-v1' or 'ecsWriteLayout' not in profile or \
                not value_bool(provenance, 'resourceAtomicScalarWriteValidated'):
            raise ProfileError('resource scalar writes lack original worker lifetime validation')
        r['enabled'] = value_bool(model, 'enabled')
        r['service_thread'] = offset(model, 'serviceThreadOffset', 65536)
        r['callback'] = offset(model, 'threadCallbackRva', image_size - 1)
        r['initialized'] = offset(model, 'serviceInitializedOffset', 65536)
        r['initialized_value'] = get_u32(model, 'serviceInitializedValue')
        r['reader'] = offset(at(profile, 'resourceLayout'), 'resourceServiceReaderOffset', 65536)
        r['payload_pointer'] = offset(model, 'servicePayloadPointerOffset', 65536)
        r['payload_size'] = offset(model, 'servicePayloadSizeOffset', 65536)
        name = text(model, 'threadName')
        if (not r['service_thread'] or not r['callback'] or r['initialized_value'] != 1 or not name or
                len(name.encode()) > w.get('name_capacity_value', 0) or '\0' in name):
            raise ProfileError('invalid resource content worker layout')
        r['name'] = name.encode()
        if r['enabled']:
            sections |= SECTION_BITS['resource_write']
    lay['resource_write'] = r
    if lay['dedicated_server'] and (not lay['ecs'] or not lay['authority'] or 'ecsWriteLayout' not in profile or
                                    lay['root_rva'] or lay['root_vtable'] or lay['client_offset']):
        raise ProfileError('dedicated server requires a validated registered Scene owner layout')
    services = profile.get('serviceLayout')
    if services is not None:
        if not w['enabled'] or not value_bool(provenance, 'servicePlainDataValidated'):
            raise ProfileError('named service data lacks original mutable/lifetime validation')
        sections |= SECTION_BITS['services']
    return lay, sections


class _Object(dict):
    """A JSON object that remembers the keys it saw more than once (json keeps the last silently)."""
    duplicates = ()


def _object(pairs):
    obj = _Object(pairs)
    if len(obj) != len(pairs):
        seen = set()
        obj.duplicates = tuple(k for k, _ in pairs if k in seen or seen.add(k))
    return obj


def _no_duplicates(obj, where):
    if isinstance(obj, dict):
        if getattr(obj, 'duplicates', ()):
            raise ProfileError('%s: duplicate key %s' % (where, obj.duplicates[0]))
        for key, value in obj.items():
            _no_duplicates(value, where + '.' + key)
    elif isinstance(obj, list):
        for i, value in enumerate(obj):
            _no_duplicates(value, '%s.%d' % (where, i))


def _exact_keys(obj, keys, where):
    if not isinstance(obj, dict):
        raise ProfileError('%s must be an object' % where)
    for key in obj:
        if key not in keys:
            raise ProfileError('%s: unknown key %s' % (where, key))
    for key in keys:
        if key not in obj:
            raise ProfileError('%s: missing key %s' % (where, key))


def _strings(value, where, non_empty=False):
    if not isinstance(value, list) or any(not isinstance(v, str) or not v for v in value) or (non_empty and not value):
        raise ProfileError('%s must be a list of non-empty strings' % where)
    return value


def parse_engine_calls(profile, image_size, write_enabled):
    """Kiln's engineCalls section -> one recipe per CallRecipes slot (None: empty), kiln_engine_layout.h CallRecipe.
    Every rule ecs_gate_invoke's call_recipe applies at run time (invoke.cpp) holds here too (its total cap of 64 KiB
    also caps each range), plus what it does not check: no two ranges overlapping (harmless there, the compare would
    only read the same bytes twice), the original bytes' digest (--check) and the review record."""
    slots = [None] * CALL_RECIPES
    if 'engineCalls' not in profile:
        return slots
    if 'engineCalls' in getattr(profile, 'duplicates', ()):
        raise ProfileError('duplicate key engineCalls')
    calls = profile['engineCalls']
    if not isinstance(calls, dict):
        raise ProfileError('engineCalls must be an object keyed by CallRecipeId name')
    _no_duplicates(calls, 'engineCalls')
    if calls and not write_enabled:
        raise ProfileError('engineCalls need the enabled ECS write gate (the call runs inside its hold)')
    for name, recipe in calls.items():
        where = 'engineCalls.' + name
        if name not in CALL_RECIPE_IDS:
            raise ProfileError('unknown engine call recipe: ' + name)
        _exact_keys(recipe, CALL_KEYS, where)
        rid = recipe['id']
        if not is_int(rid) or rid != CALL_RECIPE_IDS[name]:
            raise ProfileError('%s: id %r is not CallRecipeId::%s (%d): recipe slot/id mismatch'
                               % (where, rid, name, CALL_RECIPE_IDS[name]))
        args = recipe['argBytes']
        if (not isinstance(args, list) or len(args) > CALL_ARGUMENTS or
                any(not is_int(a) or a < 0 or a >= 1 << 32 for a in args)):
            raise ProfileError('%s: argBytes must be at most %d unsigned 32-bit sizes (0: an integer argument)'
                               % (where, CALL_ARGUMENTS))
        bits = recipe['resultBits']
        if not is_int(bits) or bits not in CALL_RESULT_BITS:
            raise ProfileError('%s: resultBits must be one of %s' % (where, CALL_RESULT_BITS))
        ranges = recipe['ranges']
        if not isinstance(ranges, list) or not ranges or len(ranges) > CALL_RANGES:
            raise ProfileError('%s: ranges must hold 1..%d ranges' % (where, CALL_RANGES))
        parsed, total = [], 0
        for i, r in enumerate(ranges):
            at = '%s.ranges.%d' % (where, i)
            _exact_keys(r, CALL_RANGE_KEYS, at)
            rva, size, digest = r['rva'], r['size'], r['original_sha256']
            if not is_int(rva) or not is_int(size) or rva < 0 or size <= 0:
                raise ProfileError('%s: rva and size must be integers, size > 0' % at)
            if rva >= image_size or size > image_size - rva:
                raise ProfileError('%s: range outside the image (SizeOfImage %d)' % (at, image_size))
            if size > CALL_CODE_BYTES:
                raise ProfileError('%s: range of %d bytes is over 64 KiB' % (at, size))
            total += size
            if total > CALL_CODE_BYTES:
                raise ProfileError('%s: ranges total over 64 KiB (kCallCodeBytes)' % where)
            if not isinstance(digest, str) or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
                raise ProfileError('%s: original_sha256 must be 64 lowercase hex digits' % at)
            for j, (other_rva, other_size, _) in enumerate(parsed):
                if rva < other_rva + other_size and other_rva < rva + size:
                    raise ProfileError('%s: range overlaps ranges.%d' % (at, j))
            parsed.append((rva, size, digest))
        review = recipe['review']
        _exact_keys(review, CALL_REVIEW_KEYS, where + '.review')
        if not isinstance(review['sfSource'], str) or not review['sfSource']:
            raise ProfileError('%s.review: sfSource must name the ShroudForge source of the call' % where)
        if review['sfInHold'] is not True:
            raise ProfileError('%s.review: sfInHold must be true (admitted only when ShroudForge calls it inside its '
                               'Scene hold on this exact build)' % where)
        for key in ('locks', 'allocators', 'stackBuffersMovedToHeap', 'boundedLoops'):
            _strings(review[key], '%s.review.%s' % (where, key))
        _exact_keys(review['threadState'], CALL_THREAD_STATE_KEYS, where + '.review.threadState')
        for key in CALL_THREAD_STATE_KEYS:
            if not isinstance(review['threadState'][key], str) or not review['threadState'][key]:
                raise ProfileError('%s.review.threadState.%s must be a non-empty string' % (where, key))
        slots[rid - 1] = {'name': name, 'id': rid, 'arg_bytes': list(args), 'result_bits': bits, 'ranges': parsed}
    return slots


def load(path):
    raw = path.read_bytes()
    profile = json.loads(raw, object_pairs_hook=_object)
    side = path.parent.name
    target = {'client': 'enshrouded.exe', 'server': 'enshrouded_server.exe'}.get(side)
    if target is None:
        raise ProfileError('%s: side folder must be client or server' % path)
    if not eligible(profile, target):
        raise ProfileError('%s: not an exact-image read-validated KFC4 profile for %s (select_resource_profile.cpp:5-13)'
                           % (path, target))
    image = at(profile, 'image')
    sha = text(image, 'sha256')
    timestamp, size = get_size(image, 'timestamp'), get_size(image, 'size')
    if len(sha) != 64 or any(c not in '0123456789abcdef' for c in sha) or timestamp >= 1 << 32 or size >= 1 << 32 or not size:
        raise ProfileError('%s: image identity must be a 64-hex sha256 and 32-bit TimeDateStamp/SizeOfImage' % path)
    if text(profile, 'id').startswith('beta_'):
        raise ProfileError('%s: beta profiles are never ported' % path)
    try:
        resource = parse_resource_layout(profile, size)
        layout, sections = configure(profile, size, target)
        calls = parse_engine_calls(profile, size, layout['write'].get('enabled', False))
    except ProfileError as error:
        raise ProfileError('%s: %s' % (path, error)) from None
    sections |= SECTION_BITS['resource_read']
    if resource['item_id_map']:
        sections |= SECTION_BITS['item_id_map']
    return {'path': path, 'raw': raw, 'profile': profile, 'target': target, 'sha256': sha, 'timestamp': timestamp,
            'size': size, 'layout': layout, 'sections': sections, 'attribute_model': attribute_model(profile),
            'resource': resource['typed'], 'calls': calls}


def attribute_model(profile):
    # initialize_resource_access.cpp:30-38 (publish_attribute_model), run once the exact profile is selected and
    # configured: value("attributeCalculationModel", "") and at("provenance") throw on a non-string or a missing object.
    model = profile.get('attributeCalculationModel', '')
    if not isinstance(model, str):
        raise ProfileError('[json.exception.type_error.302] type must be string: attributeCalculationModel')
    provenance = at(profile, 'provenance')
    if not isinstance(provenance, dict):
        raise ProfileError('[json.exception.type_error.304] cannot use at() with a non-object: provenance')
    evidence = provenance.get('attributeCalculationEvidence')
    if model != 'attribute-command-v1' or not isinstance(evidence, str) or not evidence:
        return ''
    return model


def leaves(obj, prefix=''):
    out = []
    if isinstance(obj, dict):
        for key in obj:
            if not prefix and key == 'engineCalls':
                continue  # Kiln's own section, typed as CallRecipes; the leaves stay the ShroudForge profile's
            out.extend(leaves(obj[key], prefix + ('.' if prefix else '') + key))
    elif isinstance(obj, list):
        for i, value in enumerate(obj):
            out.extend(leaves(value, prefix + ('.' if prefix else '') + str(i)))
    else:
        out.append((prefix, obj))
    return out


def c_string(value):
    data = value.encode('utf-8')
    return '"' + ''.join(chr(b) if 32 <= b < 127 and b not in (34, 92, 63) else '\\%03o' % b for b in data) + '"'


def c_bytes(data, length):
    return '{' + ', '.join(str(b) for b in data.ljust(length, b'\0')) + '}'


def call_recipes(slots):
    """The CallRecipes initializer: 'CallRecipes{}' when the profile has no engine call (every ported profile until a
    service lands its reviewed recipe), else the slots up to the last used one; unused slots and ranges stay zero."""
    used = [i for i, s in enumerate(slots) if s]
    if not used:
        return 'CallRecipes{}'
    out = []
    for s in slots[:used[-1] + 1]:
        if not s:
            out.append('CallRecipe{}')
            continue
        args = s['arg_bytes'] + [0] * (CALL_ARGUMENTS - len(s['arg_bytes']))
        out.append('CallRecipe{%du, %du, %du, %du, {%s}, {%s}}' % (
            s['id'], len(s['arg_bytes']), s['result_bits'], len(s['ranges']), ', '.join('%du' % a for a in args),
            ', '.join('CallRange{0x%xu, %du}' % (rva, size) for rva, size, _ in s['ranges'])))
    return 'CallRecipes{{%s}}' % ', '.join(out)


def emit(profiles, root):
    lines = [
        '// SPDX-License-Identifier: MIT',
        '// @generated by tools/engine/generate.py; do not edit.',
        '// Data: ShroudForge per-build engine profiles (MIT, Copyright (c) 2026 bonsaibauer), copied verbatim to',
        '// sigdb/engine/ from ' + SOURCE_PIN + ',',
        '// modules/game-runtime/profiles/enshrouded/<side>/<build>.json. Validated at generation time with ShroudForge\'s',
        '// eligibility, resource-layout and engine-layout rules (tools/engine/generate.py).',
        '#pragma once',
        '#include "include/kiln_engine_layout.h"',
        '',
        'namespace kiln::engine::generated {',
    ]
    for index, p in enumerate(profiles):
        rel = p['path'].relative_to(root).as_posix()  # relative to the selected --root, not this checkout
        lines.append('// %s sha256 %s' % (rel, hashlib.sha256(p['raw']).hexdigest()))
        lines.append('inline constexpr Leaf kLeaves%d[] = {' % index)
        for key, value in sorted(leaves(p['profile'])):
            if isinstance(value, bool):
                lines.append('    {%s, LeafKind::Bool, %du, nullptr},' % (c_string(key), int(value)))
            elif is_int(value):
                if value < 0 or value >= 1 << 64:
                    raise ProfileError('%s: %s is not a u64' % (rel, key))
                lines.append('    {%s, LeafKind::U64, %du, nullptr},' % (c_string(key), value))
            elif isinstance(value, str):
                lines.append('    {%s, LeafKind::Text, 0u, %s},' % (c_string(key), c_string(value)))
            elif value is None:
                lines.append('    {%s, LeafKind::Null, 0u, nullptr},' % c_string(key))
            else:
                raise ProfileError('%s: unsupported leaf %s' % (rel, key))
        lines.append('};')
    lines.append('inline constexpr Profile kProfiles[] = {')
    for index, p in enumerate(profiles):
        lay, w, r, e = p['layout'], p['layout']['write'], p['layout']['resource_write'], p['layout']['entities']
        q, s = lay['queue'], lay['spawn']

        def g(d, k):
            return d.get(k, 0) if d else 0
        entity = ('EcsLayout{%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %s, %d, %d, %d, %d}' %
                  (e['manager_count'], e['manager_table'], e['manager_occupancy'], e['manager_keys'], e['entity_id'],
                   e['entity_generation'], e['entity_layout'], e['entity_storage'], e['entity_definition'],
                   e['entity_row'], e['entity_definition_uuid'],
                   'true' if e['entity_definition_uuid_validated'] else 'false', e['component_bits'],
                   e['component_offsets'], e['component_strides'], e['maximum_entities']))
        write = ('WriteLayout{%s, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, '
                 '%d, %d, %d, %s, %d, %d, %d, %d, %d, %d, %d, %d, %s}' % (
                     'true' if w.get('enabled') else 'false', g(w, 'registry_lock'), g(w, 'registry_entries'),
                     g(w, 'registry_count'), g(w, 'callback'), g(w, 'capacity'), g(w, 'stride'), g(w, 'row_handle'),
                     g(w, 'row_name'), g(w, 'row_length'), g(w, 'inline_name'), g(w, 'scene_thread'),
                     g(w, 'name_pointer'), g(w, 'name_length'), g(w, 'name_capacity'), g(w, 'name_capacity_value'),
                     g(w, 'thread_handle'), g(w, 'thread_context'), g(w, 'thread_stop'), g(w, 'thread_callback'),
                     g(w, 'scene_context'), g(w, 'scene_lock'), g(w, 'scene_state'), g(w, 'execution_mode'),
                     g(w, 'maximum_bytes'), g(w, 'running_state'), str(g(w, 'authority_mode')),
                     len(w.get('name', b'')), g(w, 'tracker'), g(w, 'tracking_context'), g(w, 'tracking_manager'),
                     g(w, 'tracking_queue_b'), g(w, 'maximum_tracking_capacity'), g(w, 'tracking_registry_a'),
                     g(w, 'tracking_registry_b'),
                     c_bytes(w.get('name', b''), 64) + ', ' + c_bytes(w.get('tracker_bytes', b''), 33)))
        queue = ('QueueLayout{%s, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d}' % (
            'true' if q else 'false', g(q, 'task_state'), g(q, 'task_records'), g(q, 'task_count'),
            g(q, 'queue_records'), g(q, 'queue_count'), g(q, 'task_record_stride'), g(q, 'entity_id_counter'),
            g(q, 'active_tracking_contexts'), g(q, 'shared_tracker'), g(q, 'manager_owner'), g(q, 'execution_mode'),
            g(q, 'buffer_capacity'), g(q, 'authority_mode')))
        spawn = 'SpawnLayout{%s, %d, %du}' % ('true' if s else 'false', g(s, 'source_entity_ids'),
                                              g(s, 'allocation_failure_marker'))
        resource = ('ResourceWriteLayout{%s, %d, %d, %d, %d, %d, %du, %d, %d, %s}' % (
            'true' if r.get('enabled') else 'false', g(r, 'service_thread'), g(r, 'initialized'), g(r, 'reader'),
            g(r, 'payload_pointer'), g(r, 'payload_size'), g(r, 'initialized_value'), g(r, 'callback'),
            len(r.get('name', b'')), c_bytes(r.get('name', b''), 64)))
        rt = p['resource']
        typed = 'ResourceLayout{%s, ItemMapLayout{%s, %s}}' % (
            ', '.join('%du' % rt[field] for field, _ in RESOURCE_FIELDS),
            'true' if rt['item_map']['enabled'] else 'false',
            ', '.join('%du' % rt['item_map'][field] for field, _ in ITEM_MAP_FIELDS))
        layout = ('Layout{%s, %d, %d, %d, %d, %d, %d, %d, %d, %d, %s, %d, %d, %d, %d, %s, %d, %d, %d, %d, %d, %d, %d, %d, '
                  '%d, %d, %s, %s, %s, %s, %s, %s, %s}' % (
                      'true' if lay['dedicated_server'] else 'false', lay['image_size'], lay['root_rva'],
                      lay['root_vtable'], lay['client_vtable'], lay['controller_vtable'], lay['client_offset'],
                      lay['controller_offset'], lay['controller_back_pointer'], lay['main_owner_offset'],
                      'true' if lay['ecs'] else 'false', lay['execution_owner_offset'], lay['registry_owner_offset'],
                      lay['entity_manager_offset'], lay['manager_owner_offset'],
                      'true' if lay['authority'] else 'false', lay['client_scene_offset'], lay['scene_entries_offset'],
                      lay['scene_count_offset'], lay['scene_capacity_offset'], lay['entry_stride'],
                      lay['entry_context_offset'], lay['entry_uuid_offset'], lay['entry_state_offset'],
                      lay['maximum_scenes'], lay['entry_ready_state'], entity, write, queue, spawn, resource, typed,
                      call_recipes(p['calls'])))
        lines.append('    {%s, %s, %s, %du, %du, %du, %du, 0x%xu, %s, kLeaves%d, sizeof kLeaves%d / sizeof kLeaves%d[0], %s},' % (
            c_string(text(p['profile'], 'id')), c_string(p['target']), c_string(p['sha256']), p['timestamp'],
            p['size'], 4, p['profile']['schemaVersion'], p['sections'], layout, index, index, index,
            c_string(p['attribute_model'])))
    lines.append('};')
    lines.append('}  // namespace kiln::engine::generated')
    return '\n'.join(lines) + '\n'


def pe_sections(data):
    """(SizeOfImage, [(name, va, virtual size, file offset, raw size)]) of an AMD64 PE32+ image file."""
    if len(data) < 0x40 or data[:2] != b'MZ':
        raise ProfileError('not a PE image')
    at = struct.unpack_from('<I', data, 0x3c)[0]
    if data[at:at + 4] != b'PE\0\0':
        raise ProfileError('not a PE image')
    machine, count = struct.unpack_from('<HH', data, at + 4)
    optional_size = struct.unpack_from('<H', data, at + 20)[0]
    optional = at + 24
    if machine != 0x8664 or struct.unpack_from('<H', data, optional)[0] != 0x20b:
        raise ProfileError('not an AMD64 PE32+ image')
    size_of_image = struct.unpack_from('<I', data, optional + 56)[0]
    sections = []
    for i in range(count):
        s = optional + optional_size + 40 * i
        name = data[s:s + 8].rstrip(b'\0').decode('latin-1')
        vsize, va, rsize, raw = struct.unpack_from('<IIII', data, s + 8)
        sections.append((name, va, vsize, raw, rsize))
    return size_of_image, sections


# IMAGE_REL_BASED_* -> the bytes the fixup rewrites (0, ABSOLUTE, is padding). Any other type fails the check closed.
RELOCATION_WIDTHS = {1: 2, 2: 2, 3: 4, 10: 8}


def pe_relocations(data, sections):
    """The base relocations of an AMD64 PE32+ image file: a sorted list of (rva of the field, bytes it rewrites)."""
    at = struct.unpack_from('<I', data, 0x3c)[0]
    optional = at + 24
    optional_size = struct.unpack_from('<H', data, at + 20)[0]
    if optional_size < 112 + 8 * 6 or struct.unpack_from('<I', data, optional + 108)[0] <= 5:
        return []
    rva, size = struct.unpack_from('<II', data, optional + 112 + 8 * 5)
    if not size:
        return []
    start = None
    for _, va, vsize, raw, rsize in sections:
        if va <= rva and rva + size <= va + min(vsize, rsize) and raw + (rva - va) + size <= len(data):
            start = raw + rva - va
            break
    if start is None:
        raise ProfileError('the base relocation directory is not inside one section\'s file data')
    out, at, end = [], start, start + size
    while at < end:
        page, block = struct.unpack_from('<II', data, at)
        if block < 8 or block % 2 or at + block > end:
            raise ProfileError('malformed base relocation block at file offset 0x%x' % at)
        for i in range((block - 8) // 2):
            entry = struct.unpack_from('<H', data, at + 8 + 2 * i)[0]
            kind = entry >> 12
            if kind == 0:
                continue
            if kind not in RELOCATION_WIDTHS:
                raise ProfileError('base relocation type %d at rva 0x%x is not supported' % (kind, page + (entry & 0xfff)))
            out.append((page + (entry & 0xfff), RELOCATION_WIDTHS[kind]))
        at += block
    return sorted(out)


def file_bytes(data, sections, rva, size):
    """The file bytes an image range maps, or None when the range is not wholly inside .text's file data: kiln.hooks'
    read_original, which a calling member checks original_sha256 on, reads only the exe's .text (the loader zero-fills
    a virtual size beyond the raw data, which is no file byte)."""
    for name, va, vsize, raw, rsize in sections:
        if name != '.text':
            continue
        mapped = min(vsize, rsize) if vsize else rsize
        if va <= rva and rva + size <= va + mapped and raw + (rva - va) + size <= len(data):
            return data[raw + rva - va: raw + rva - va + size]
    return None


def original_checks(profiles, binaries):
    """Each recipe range's original_sha256 against a read-only copy of its exact executable under `binaries` (any file
    named like the target whose sha256 is the profile's; never the installed game, never run). Returns [(ok, line)]:
    ok None when no copy of that build is there (not checked). A range must lie inside .text's file data and hold no
    base relocation: read_original gives the file's bytes relocated to where the exe is loaded, and the member checks
    original_sha256 on those, so only relocation-free bytes have one digest at every load address."""
    out = []
    for p in profiles:
        recipes = [s for s in p['calls'] if s]
        if not recipes:
            continue
        copy = None
        for candidate in sorted(binaries.rglob(p['target'])):
            if candidate.is_file():
                data = candidate.read_bytes()
                if hashlib.sha256(data).hexdigest() == p['sha256']:
                    copy = data
                    break
        if copy is None:
            out.append((None, '%s: no copy of %s %s under %s' % (p['path'].name, p['target'], p['sha256'][:12], binaries)))
            continue
        size_of_image, sections = pe_sections(copy)
        if size_of_image != p['size']:
            out.append((False, '%s: the copy\'s SizeOfImage %d is not the profile\'s %d'
                        % (p['path'].name, size_of_image, p['size'])))
            continue
        relocations = pe_relocations(copy, sections)
        for s in recipes:
            for i, (rva, size, digest) in enumerate(s['ranges']):
                where = '%s %s ranges.%d (rva 0x%x, %d bytes)' % (p['path'].name, s['name'], i, rva, size)
                data = file_bytes(copy, sections, rva, size)
                # every fixup field that starts before the range's end and ends after its start (fields are at most 8
                # bytes long, so none starting before rva - 8 can reach it)
                low = bisect.bisect_left(relocations, (rva - 8, 0))
                high = bisect.bisect_left(relocations, (rva + size, 0))
                fixups = [(r, w) for r, w in relocations[low:high] if r + w > rva]
                if data is None:
                    out.append((False, where + ': not inside .text\'s file data'))
                elif fixups:
                    out.append((False, where + ': holds a base relocation (rva 0x%x, %d bytes): read_original\'s '
                                'relocated bytes differ from the file\'s at another load address' % fixups[0]))
                elif hashlib.sha256(data).hexdigest() != digest:
                    out.append((False, where + ': original bytes differ from original_sha256'))
                else:
                    out.append((True, where + ': original bytes match'))
    return out


def load_all(root):
    paths = sorted((root / 'sigdb/engine').glob('*/*.json'))
    if not paths:
        raise ProfileError('no profiles under sigdb/engine')
    return [load(p) for p in paths]


def generate(root, profiles=None):
    if profiles is None:
        profiles = load_all(root)
    seen = set()
    for p in profiles:
        key = (p['target'], p['sha256'], p['timestamp'], p['size'])
        if key in seen:
            raise ProfileError('two ported profiles match one image: %s' % (key,))
        seen.add(key)
    return emit(profiles, root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--binaries', type=Path, default=os.environ.get('KILN_ENGINE_BINARIES') or None,
                        help='--check: a folder of read-only exe copies to check each engine call range\'s '
                             'original_sha256 against (default: $KILN_ENGINE_BINARIES)')
    args = parser.parse_args()
    try:
        root = args.root.resolve()
        profiles = load_all(root)
        content = generate(root, profiles)
    except (ProfileError, OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 2
    target = args.root / OUTPUT
    if args.check:
        if not target.is_file() or target.read_text(encoding='utf-8') != content:
            print('drift: ' + OUTPUT.as_posix(), file=sys.stderr)
            return 1
        print('checked engine profiles: ' + OUTPUT.as_posix())
        recipes = [(p, s) for p in profiles for s in p['calls'] if s]
        if recipes and args.binaries is None:
            # never silent: a recipe's original bytes are either checked against a copy or said to be unchecked
            print('engine call originals: NOT CHECKED, no --binaries or KILN_ENGINE_BINARIES (%d ranges in %d profiles)'
                  % (sum(len(s['ranges']) for _, s in recipes), len({id(p) for p, _ in recipes})))
        if args.binaries is not None:
            if not Path(args.binaries).is_dir():
                print('no such folder: %s' % args.binaries, file=sys.stderr)
                return 2
            try:
                checks = original_checks(profiles, Path(args.binaries))
            except (ProfileError, OSError, struct.error) as error:
                print(str(error), file=sys.stderr)
                return 2
            for ok, line in checks:
                print(('ok   ' if ok else 'FAIL ' if ok is False else 'NOT CHECKED ') + line)
            print('engine call originals: %d checked, %d failed, %d profiles NOT CHECKED (no copy of their build)'
                  % (sum(1 for ok, _ in checks if ok is not None), sum(1 for ok, _ in checks if ok is False),
                     sum(1 for ok, _ in checks if ok is None)))
            if any(ok is False for ok, _ in checks):
                return 1
        return 0
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding='utf-8', newline='\n')
    print('generated engine profiles: ' + OUTPUT.as_posix())
    return 0


if __name__ == '__main__':
    sys.exit(main())
