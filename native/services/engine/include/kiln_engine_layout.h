// SPDX-License-Identifier: MIT
// kiln.engine's validated per-build layout (internal; filled only by the generated profiles.g.h).
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer),
// modules/game-runtime/native/engine-context/engine_context_state.h:21-63 (WriteLayout, ResourceWriteLayout, Layout)
// and engine_owners.h:26-48 (EcsLayout, EntityCommandQueueLayout, BuildingPropSpawnLayout). Field order is the
// generator's emit order (tools/engine/generate.py); every value passed ShroudForge's Configure rules there.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace kiln::engine {

struct EcsLayout {
    std::size_t manager_count, manager_table, manager_occupancy, manager_keys;
    std::size_t entity_id, entity_generation, entity_layout, entity_storage, entity_definition, entity_row;
    std::size_t entity_definition_uuid;
    bool entity_definition_uuid_validated;
    std::size_t component_bits, component_offsets, component_strides, maximum_entities;
};

struct WriteLayout {
    bool enabled;
    std::uintptr_t registry_lock, registry_entries, registry_count, callback;
    std::size_t capacity, stride, row_handle, row_name, row_length;
    std::size_t inline_name, scene_thread, name_pointer, name_length, name_capacity, name_capacity_value;
    std::size_t thread_handle, thread_context, thread_stop, thread_callback;
    std::size_t scene_context, scene_lock, scene_state, execution_mode, maximum_bytes;
    std::uint8_t running_state, authority_mode;
    std::size_t name_size;
    std::uintptr_t tracker;
    std::size_t tracking_context, tracking_manager, tracking_queue_b, maximum_tracking_capacity;
    std::size_t tracking_registry_a, tracking_registry_b;
    std::array<char, 64> name;
    std::array<std::uint8_t, 33> tracker_bytes;
};

struct QueueLayout {
    bool present;
    std::size_t task_state, task_records, task_count, queue_records, queue_count, task_record_stride;
    std::size_t entity_id_counter, active_tracking_contexts, shared_tracker, manager_owner, execution_mode;
    std::uint64_t buffer_capacity;
    std::uint8_t authority_mode;
};

struct SpawnLayout {
    bool present;
    std::size_t source_entity_ids;
    std::uint32_t allocation_failure_marker;
};

struct ResourceWriteLayout {
    bool enabled;
    std::size_t service_thread, initialized, reader, payload_pointer, payload_size;
    std::uint32_t initialized_value;
    std::uintptr_t callback;
    std::size_t name_size;
    std::array<char, 64> name;
};

struct Layout {
    bool dedicated_server;
    std::size_t image_size;
    std::uintptr_t root_rva, root_vtable, client_vtable, controller_vtable;
    std::size_t client_offset, controller_offset, controller_back_pointer, main_owner_offset;
    bool ecs;
    std::size_t execution_owner_offset, registry_owner_offset, entity_manager_offset, manager_owner_offset;
    bool authority;
    std::size_t client_scene_offset, scene_entries_offset, scene_count_offset, scene_capacity_offset;
    std::size_t entry_stride, entry_context_offset, entry_uuid_offset, entry_state_offset, maximum_scenes;
    std::uint8_t entry_ready_state;
    EcsLayout entities;
    WriteLayout write;
    QueueLayout queue;
    SpawnLayout spawn;
    ResourceWriteLayout resource_write;
};

// The verbatim profile's leaves (JSON path -> value), sorted by key, for diagnostics and later typed records.
enum class LeafKind : std::uint8_t { U64, Bool, Text, Null };
struct Leaf {
    const char* key;
    LeafKind kind;
    std::uint64_t value;
    const char* text;
};

struct Profile {
    const char* id;
    const char* target;
    const char* sha256;
    std::uint32_t timestamp, size_of_image, archive_format, schema_version, sections;
    Layout layout;
    const Leaf* leaves;
    std::size_t leaf_count;
    // ShroudForge's published attribute model for this exact profile (initialize_resource_access.cpp:30-38):
    // "attribute-command-v1" when the profile names it with non-empty provenance.attributeCalculationEvidence, else "".
    const char* attribute_calculation_model;
};

}  // namespace kiln::engine
