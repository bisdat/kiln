// SPDX-License-Identifier: MIT
// Owner, registered-Scene and ECS view resolution (any thread; no allocation, no host lock, no wait).
#pragma once
#include <cstddef>
#include <cstdint>

#include "include/kiln_engine_layout.h"
#include "include/kiln_engine_view.h"

namespace kiln::engine {

struct RegisteredScene {
    std::uintptr_t scene, thread, handle;
    bool operator==(const RegisteredScene&) const = default;
};

// Caller holds the thread-registry word Shared. Borrows the one live "GameServer" Scene.
Reason resolve_registered_scene(const Layout& layout, std::uintptr_t image, RegisteredScene& out);
Reason resolve_owners(const Layout& layout, Owners& out, std::uintptr_t image);
Reason select_execution_owner(const Layout& layout, EcsView& view, std::uintptr_t image);
Reason resolve_ecs(const Layout& layout, EcsView& out, std::uintptr_t image);
bool untracked_component(std::uintptr_t owner, std::uint16_t index, std::size_t mask_a, std::size_t mask_b);

enum class ViewKind : std::uint32_t { Authority = 0, Client = 1 };
// ShroudForge ResolveEcs (Authority) and ResolveClientEcs (Client); layout NULL = no exact profile.
Reason ecs_view(const Layout* layout, ViewKind kind, EcsView& out, std::uintptr_t image);
// ShroudForge EcsComponentWriteSupported.
bool ecs_write_supported(const Layout* layout, const EcsView& expected, std::uint16_t index, std::uintptr_t image);

}  // namespace kiln::engine
