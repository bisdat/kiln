// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer):
//   modules/game-runtime/native/engine-context/resolve_engine_owners.cpp:32-200 (select_scene_context, resolve,
//     select_execution_owner, resolve_ecs) and :221-243 (ResolveEcs, ResolveClientEcs);
//   resolve_registered_scene.cpp:4-74; lock_scene_access.cpp:6-19 and :68-82 (untracked_component,
//   EcsComponentWriteSupported).
// Same order of reads and checks, same refusals (Reason = ShroudForge's string, kiln_engine_view.h). The profile is
// Kiln's compiled-in validated layout instead of a context-mutex snapshot; there is nothing to lock.
#include "resolve.h"

#include <cstring>

#include "memory.h"

namespace kiln::engine {
using mem::add;
using mem::field;
using mem::read;
using mem::read_span;

namespace {
Reason select_scene_context(const Layout& layout, EcsView& view) {
    std::uint64_t count{}, capacity{};
    if (!field(view.scene_owner, layout.scene_entries_offset, view.scene_entries) ||
        !field(view.scene_owner, layout.scene_count_offset, count) || count > layout.maximum_scenes ||
        !field(view.scene_owner, layout.scene_capacity_offset, capacity) || capacity < count ||
        capacity > layout.maximum_scenes)
        return Reason::EcsActiveSceneListUnavailable;
    if (count && (!view.scene_entries || view.scene_entries > UINTPTR_MAX - count * layout.entry_stride))
        return Reason::EcsActiveSceneListRangeInvalid;
    std::size_t ready{};
    for (std::size_t index = 0; index < count; ++index) {
        const auto entry = view.scene_entries + index * layout.entry_stride;
        std::uint8_t state{};
        if (!field(entry, layout.entry_state_offset, state)) return Reason::EcsSceneStateUnreadable;
        if (state != layout.entry_ready_state) continue;
        if (++ready > 1) return Reason::EcsLocalAuthorityIsAmbiguous;
        view.scene_entry = entry;
        if (!field(entry, layout.entry_context_offset, view.scene_context) || !view.scene_context ||
            !field(entry, layout.entry_uuid_offset, view.scene_uuid) || !(view.scene_uuid[0] || view.scene_uuid[1]))
            return Reason::EcsReadySceneContextInvalid;
    }
    if ((count || layout.dedicated_server) && !ready) return Reason::EcsActiveSceneIsTransitioning;
    if (ready) {
        std::uint8_t state{};
        std::uintptr_t context{};
        std::array<std::uint64_t, 2> uuid{};
        if (!field(view.scene_entry, layout.entry_state_offset, state) || state != layout.entry_ready_state ||
            !field(view.scene_entry, layout.entry_context_offset, context) || context != view.scene_context ||
            !field(view.scene_entry, layout.entry_uuid_offset, uuid) || uuid != view.scene_uuid)
            return Reason::EcsReadySceneContextChanged;
        view.role = Role::LocalAuthority;
    }
    std::uintptr_t entries_again{};
    std::uint64_t count_again{}, capacity_again{};
    if (!field(view.scene_owner, layout.scene_entries_offset, entries_again) || entries_again != view.scene_entries ||
        !field(view.scene_owner, layout.scene_count_offset, count_again) || count_again != count ||
        !field(view.scene_owner, layout.scene_capacity_offset, capacity_again) || capacity_again != capacity)
        return Reason::EcsActiveSceneListChanged;
    return Reason::None;
}
}  // namespace

Reason resolve_registered_scene(const Layout& layout, std::uintptr_t image, RegisteredScene& output) {
    const auto& w = layout.write;
    if (!image || !w.capacity || !w.stride || !w.name_size || w.name_size > w.name.size())
        return Reason::EcsServerThreadLayoutInvalid;
    std::uint16_t count{};
    if (!field(image, w.registry_count, count) || count > w.capacity) return Reason::EcsThreadRegistryCountInvalid;
    std::uintptr_t entries{};
    if (!add(image, w.registry_entries, entries)) return Reason::EcsThreadRegistryRangeInvalid;
    RegisteredScene selected{};
    std::uintptr_t selected_row{}, selected_name{};
    unsigned matches{};
    for (std::size_t index = 0; index < count; ++index) {
        std::uintptr_t row{}, name{}, handle{};
        std::uint64_t length{};
        if (index > UINTPTR_MAX / w.stride || !add(entries, index * w.stride, row) || !field(row, w.row_length, length) ||
            !field(row, w.row_name, name) || !field(row, w.row_handle, handle))
            return Reason::EcsThreadRegistryRowUnreadable;
        if (length != w.name_size) continue;
        std::array<char, 64> text{};
        if (!read_span(name, text.data(), length)) return Reason::EcsThreadNameUnreadable;
        if (std::memcmp(text.data(), w.name.data(), length)) continue;
        if (++matches > 1) return Reason::EcsServerThreadIsAmbiguous;
        if (!handle || name < w.inline_name || name - w.inline_name < w.scene_thread) return Reason::EcsServerThreadRangeInvalid;
        selected.thread = name - w.inline_name;
        selected.scene = selected.thread - w.scene_thread;
        selected.handle = handle;
        selected_row = row;
        selected_name = name;
    }
    if (!matches) return Reason::EcsServerThreadUnregistered;
    const auto valid_thread = [&]() {
        std::uintptr_t name{}, handle{}, callback{}, context{}, wanted_context{}, wanted_callback{}, inline_name{};
        std::uint64_t length{}, capacity{};
        std::uint32_t stop{};
        std::uint8_t state{};
        return add(selected.thread, w.inline_name, inline_name) && field(selected.thread, w.name_pointer, name) &&
               name == inline_name && name == selected_name && field(selected.thread, w.name_length, length) &&
               length == w.name_size && field(selected.thread, w.name_capacity, capacity) &&
               capacity == w.name_capacity_value && field(selected.thread, w.thread_handle, handle) &&
               handle == selected.handle && add(image, w.callback, wanted_callback) &&
               field(selected.thread, w.thread_callback, callback) && callback == wanted_callback &&
               add(selected.scene, w.scene_context, wanted_context) && field(selected.thread, w.thread_context, context) &&
               context == wanted_context && field(selected.thread, w.thread_stop, stop) && stop == 0 &&
               field(selected.scene, w.scene_state, state) && state == w.running_state;
    };
    if (!valid_thread()) return Reason::EcsServerThreadOwnerInvalidOrStopping;
    std::uint16_t count_again{};
    std::uintptr_t name_again{}, handle_again{};
    std::uint64_t length_again{};
    std::array<char, 64> text_again{};
    if (!field(image, w.registry_count, count_again) || count_again != count ||
        !field(selected_row, w.row_name, name_again) || name_again != selected_name ||
        !field(selected_row, w.row_handle, handle_again) || handle_again != selected.handle ||
        !field(selected_row, w.row_length, length_again) || length_again != w.name_size ||
        !read_span(name_again, text_again.data(), length_again) ||
        std::memcmp(text_again.data(), w.name.data(), length_again) || !valid_thread())
        return Reason::EcsServerThreadChangedDuringRead;
    output = selected;
    return Reason::None;
}

Reason resolve_owners(const Layout& layout, Owners& output, std::uintptr_t image) {
    Owners view{};
    view.image = image;
    if (layout.dedicated_server) {
        std::uintptr_t registry_lock{};
        if (!view.image || !add(view.image, layout.write.registry_lock, registry_lock) || !mem::try_shared(registry_lock))
            return Reason::EcsThreadRegistryBusy;
        RegisteredScene scene{};
        EcsView world{};
        Reason failure = resolve_registered_scene(layout, view.image, scene);
        if (failure == Reason::None) {
            world.scene_owner = scene.scene;
            failure = select_scene_context(layout, world);
        }
        // Registry ownership protects the Scene allocation during traversal (resolve_engine_owners.cpp:110-111).
        mem::release_shared(registry_lock);
        if (failure != Reason::None) return failure;
        view.dedicated_server = 1;
        view.main_owner = scene.scene;
        view.world_context = world.scene_context;
        view.world_uuid = world.scene_uuid;
        output = view;
        return Reason::None;
    }
    if (!view.image || !add(view.image, layout.root_rva, view.root) ||
        !mem::vtable(view.root, layout.root_vtable, view.image, layout.image_size))
        return Reason::RootVtableMismatch;
    std::uintptr_t at{}, back{};
    if (!add(view.root, layout.client_offset, at) || !read(at, view.client) ||
        !mem::vtable(view.client, layout.client_vtable, view.image, layout.image_size))
        return Reason::ClientOwnerUnavailableOrVtableMismatch;
    if (!add(view.client, layout.controller_offset, at) || !read(at, view.controller) ||
        !mem::vtable(view.controller, layout.controller_vtable, view.image, layout.image_size))
        return Reason::ClientControllerUnavailableOrVtableMismatch;
    if (!add(view.controller, layout.controller_back_pointer, at) || !read(at, back) || back != view.client)
        return Reason::ClientControllerOwnerMismatch;
    if (!add(view.controller, layout.main_owner_offset, at) || !read(at, view.main_owner) || !view.main_owner)
        return Reason::MainOwnerUnavailable;
    output = view;
    return Reason::None;
}

Reason select_execution_owner(const Layout& layout, EcsView& view, std::uintptr_t image) {
    if (const Reason r = resolve_owners(layout, view.owners, image); r != Reason::None) return r;
    auto context = view.owners.main_owner;
    std::uintptr_t address{};
    if (layout.authority) {
        if (layout.dedicated_server) view.scene_owner = view.owners.main_owner;
        else if (!field(view.owners.client, layout.client_scene_offset, view.scene_owner) || !view.scene_owner)
            return Reason::EcsActiveSceneListUnavailable;
        if (const Reason r = select_scene_context(layout, view); r != Reason::None) return r;
        if (layout.dedicated_server &&
            (view.scene_context != view.owners.world_context || view.scene_uuid != view.owners.world_uuid))
            return Reason::EcsServerWorldOwnerChanged;
        if (view.role == Role::LocalAuthority) context = view.scene_context;
    }
    if (!add(context, layout.execution_owner_offset, address) || !read(address, view.execution_owner) || !view.execution_owner)
        return Reason::EcsExecutionOwnerUnavailable;
    return Reason::None;
}

Reason resolve_ecs(const Layout& layout, EcsView& output, std::uintptr_t image) {
    EcsView view{};
    std::uintptr_t manager_owner{}, table{};
    std::uint64_t count{};
    if (const Reason r = select_execution_owner(layout, view, image); r != Reason::None) return r;
    if (!add(view.execution_owner, layout.registry_owner_offset, view.registry_owner) ||
        !add(view.execution_owner, layout.entity_manager_offset, view.manager) ||
        !field(view.manager, layout.manager_owner_offset, manager_owner) || manager_owner != view.registry_owner)
        return Reason::EcsManagerRegistryOwnerMismatch;
    if (!field(view.manager, layout.entities.manager_count, count) || !count || count > layout.entities.maximum_entities ||
        !field(view.manager, layout.entities.manager_table, table) || !table)
        return Reason::EcsEntityTableUnavailable;
    if (table > UINTPTR_MAX - count * sizeof(std::uintptr_t)) return Reason::EcsEntityTableRangeOverflow;
    view.entity_table = table;
    EcsView again{};
    std::uintptr_t owner_again{}, table_again{};
    std::uint64_t count_again{};
    // ShroudForge reports any failure of the second pass as a change during the read (resolve_engine_owners.cpp:185-191).
    if (select_execution_owner(layout, again, image) != Reason::None || again.owners != view.owners || again.role != view.role || again.scene_owner != view.scene_owner ||
        again.scene_entries != view.scene_entries || again.scene_entry != view.scene_entry ||
        again.scene_context != view.scene_context || again.scene_uuid != view.scene_uuid ||
        again.execution_owner != view.execution_owner || !field(view.manager, layout.manager_owner_offset, owner_again) ||
        owner_again != view.registry_owner || !field(view.manager, layout.entities.manager_count, count_again) ||
        count_again != count || !field(view.manager, layout.entities.manager_table, table_again) || table_again != table)
        return Reason::EcsEngineOwnerChangedDuringRead;
    output = view;
    return Reason::None;
}

bool untracked_component(std::uintptr_t owner, std::uint16_t index, std::size_t mask_a, std::size_t mask_b) {
    std::uint64_t count{}, count_again{}, a{}, b{}, a_again{}, b_again{};
    const auto word = static_cast<std::size_t>(index / 64) * sizeof(std::uint64_t);
    if (!owner || !mask_a || !mask_b || !field(owner, 16, count) || !count || count > 1024 || index >= count ||
        !field(owner, mask_a + word, a) || !field(owner, mask_b + word, b) || !field(owner, mask_a + word, a_again) ||
        !field(owner, mask_b + word, b_again) || !field(owner, 16, count_again) || count_again != count || a_again != a ||
        b_again != b)
        return false;
    return ((a | b) & (std::uint64_t{1} << (index % 64))) == 0;
}

Reason ecs_view(const Layout* layout, ViewKind kind, EcsView& out, std::uintptr_t image) {
    if (!layout || !layout->ecs) return Reason::EcsReadOwnerProfileUnavailable;
    if (kind == ViewKind::Authority) return resolve_ecs(*layout, out, image);
    if (layout->dedicated_server) return Reason::DedicatedServerHasNoLocalClientInput;
    Layout client = *layout;  // ResolveClientEcs: the client's main world, never the hosted authority scene
    client.authority = false;
    return resolve_ecs(client, out, image);
}

bool ecs_write_supported(const Layout* layout, const EcsView& expected, std::uint16_t index, std::uintptr_t image) {
    if (!layout || !layout->write.enabled) return false;
    if (expected.role != Role::LocalAuthority) return false;
    EcsView current{}, again{};
    const auto& w = layout->write;
    return resolve_ecs(*layout, current, image) == Reason::None && current == expected &&
           untracked_component(current.registry_owner, index, w.tracking_registry_a, w.tracking_registry_b) &&
           resolve_ecs(*layout, again, image) == Reason::None && again == current &&
           untracked_component(again.registry_owner, index, w.tracking_registry_a, w.tracking_registry_b);
}

}  // namespace kiln::engine
