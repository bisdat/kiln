// SPDX-License-Identifier: MIT
// kiln.engine's validated per-build layout (internal; filled only by the generated profiles.g.h).
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer),
// modules/game-runtime/native/engine-context/engine_context_state.h:21-63 (WriteLayout, ResourceWriteLayout, Layout),
// engine_owners.h:26-48 (EcsLayout, EntityCommandQueueLayout, BuildingPropSpawnLayout) and
// loaded-resources/loaded_resource_state.h:40-65 (ItemMapLayout, the resource Layout). Field order is the
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

// The profile's resourceLayout, typed (Kiln-internal). Adapted from ShroudForge (MIT, Copyright
// (c) 2026 bonsaibauer), modules/game-runtime/native/loaded-resources/loaded_resource_state.h:40-65 (ItemMapLayout,
// Layout) and parse_resource_layout.cpp:10-110 (parse_layout); the same fields in the same order, without its
// profile_id and dedicated_server (Profile.id, Layout.dedicated_server). Every selected profile has one: the generator
// refuses a profile whose resourceLayout ShroudForge would refuse. resource_service_reader_offset is the reader field
// the content worker's lifetime guard checks (it equals resource_write.reader, which ShroudForge's Configure copies
// from it); the payload fields the guard bounds are resource_write's. item_map.enabled is false without an itemIdMap.
struct ItemMapLayout {
    bool enabled;
    std::uint32_t type_hash, part_index;
    std::size_t root_size, item_id_offset, guid_offset, map_offset, header_size;
    std::size_t bits_offset, capacity_offset, keys_offset, values_offset, count_offset;
    std::size_t maximum_capacity, maximum_probes;
};

struct ResourceLayout {
    std::size_t main_active_context_offset, context_item_registry_offset;
    std::size_t item_registry_service_offset, item_registry_handle_offset;
    std::size_t item_registry_resource_pointer_offset;
    std::size_t resource_service_reader_offset, resource_service_chunk_table_offset;
    std::size_t resource_service_id_callback_offset, resource_service_handle_callback_offset;
    std::uintptr_t resource_service_id_callback_rva, resource_service_handle_callback_rva;
    std::size_t resource_service_chunk_count_offset, reader_metadata_offset, metadata_map_offset;
    std::size_t bucket_rel32_offset, bucket_count_offset, keys_rel32_offset, key_count_offset;
    std::size_t values_rel32_offset, value_count_offset, map_header_size;
    std::size_t bucket_stride, key_stride, resource_key_size, value_stride;
    std::size_t resource_offset_offset, packed_size_offset, chunk_index_offset, size_mask;
    std::size_t chunk_stride, chunk_base_pointer_offset, chunk_size_offset;
    std::size_t maximum_entries, maximum_chunks, maximum_resource_bytes;
    ItemMapLayout item_map;
};

// The reviewed engine calls of this exact build (Kiln-internal), made by ecs_gate_invoke inside a
// held write gate. A recipe names one engine function whose whole call graph was reviewed: ranges[0] starts at the
// called entry, the other ranges are every procedure the graph reaches (and the root's linkage when the review keeps
// it). The review records every engine lock and allocator the graph touches; a graph is admitted only when ShroudForge
// calls it inside its Scene hold on this exact build, and never with a data-dependent unbounded loop. Recipe `id` is
// its slot + 1 (0: an empty slot), the same kind of call on every build. Ported profiles carry none (an empty
// record): the services that call one (prop spawn and destroy, the Impact operation) add their reviewed recipes with the generator, in the profile's
// engineCalls section (tools/engine/generate.py; review checklist: docs/kiln/services/engine-call-recipes.md).
inline constexpr std::size_t kCallRecipes = 8, kCallRanges = 32, kCallArguments = 8;
// The recipe ids, frozen: the same id names the same kind of call on every build, and an engineCalls entry is keyed by
// its name (generate.py CALL_RECIPE_IDS, compared with this enum by test_generate.py). New ids are appended.
enum class CallRecipeId : std::uint32_t {
    PropSpawnSubmit = 1,    // the building-prop spawn's engine submit
    PropDestroySubmit = 2,  // the building-prop destroy's engine submit
    ImpactSameEntity = 3,   // the Impact registered operation on its own entity
};
static_assert(static_cast<std::size_t>(CallRecipeId::ImpactSameEntity) <= kCallRecipes, "every id has a slot");
inline constexpr std::size_t kCallCodeBytes = 64 * 1024;  // the bytes of all of a recipe's ranges together, at most
struct CallRange {
    std::uintptr_t rva;
    std::size_t size;
};
struct CallRecipe {
    std::uint32_t id;           // slot + 1; 0: empty
    std::uint32_t arg_count;    // 0..kCallArguments integer or pointer arguments (x64: rcx, rdx, r8, r9, then the stack)
    std::uint32_t result_bits;  // 0 (nothing), 8, 16, 32 or 64: the low bits of rax the call answers; the rest is masked off
    std::uint32_t range_count;  // 1..kCallRanges
    // per argument: 0 an integer (passed as it is); > 0 a pointer to a caller buffer of at least that many bytes
    std::array<std::uint32_t, kCallArguments> arg_bytes;
    std::array<CallRange, kCallRanges> ranges;  // unused ranges are zero
};
struct CallRecipes {
    std::array<CallRecipe, kCallRecipes> recipe;
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
    ResourceLayout resource;  // last, so the generator's existing initializers are unchanged
    CallRecipes calls;        // after it, for the same reason
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
