// SPDX-License-Identifier: MIT
// The part of grid_for_session that runs while kiln.build holds the engine's write gate (voxel_tasks.h). No wait,
// allocation, lock, log or KilnApi call in this file, and no loop: two snapshots of a fixed set of guarded copies.
// Adapted from ShroudForge's read_snapshot (MIT, Copyright (c) 2026 bonsaibauer;
// modules/game-runtime/native/voxels/read_registered_voxel_grid.cpp:101-141).
#include <cmath>
#include <cstring>

#include "voxel_tasks.h"

namespace kb::tasks {

#ifdef KILN_BUILD_HOLD_TEST_POINTS
void (*g_hold_test_point)(HoldPoint) = nullptr;
#define KILN_HOLD_POINT(p)                           \
    do {                                             \
        if (g_hold_test_point) g_hold_test_point(p); \
    } while (0)
#else
#define KILN_HOLD_POINT(p) \
    do {                   \
    } while (0)
#endif

namespace {
uintptr_t at(uint64_t base, int64_t offset) noexcept {
    return base && offset >= 0 ? static_cast<uintptr_t>(base) + static_cast<uintptr_t>(offset) : 0;
}

Held copy_snapshot(const HoldInput& in, Snapshot& s) noexcept {
    const Offsets& o = *in.offsets;
    const auto copy = [&](uintptr_t address, void* out, size_t bytes) {
        return address && in.copy(address, out, bytes) == KILN_OK;
    };
    if (!copy(at(in.grid, o.grid_iface), s.iface, sizeof s.iface)) return Held::Unreadable;
    if (!s.iface[0]) return Held::NotReady;
    if (!copy(at(s.iface[0], o.wrapper_world), &s.voxels, sizeof s.voxels)) return Held::Unreadable;
    if (!s.voxels) return Held::NotReady;
    if (!copy(at(s.voxels, o.world_init), &s.initialized, sizeof s.initialized) ||
        !copy(at(s.voxels, o.world_origin), s.origin, sizeof s.origin) ||
        !copy(at(s.voxels, o.world_levels + o.level * o.level_stride), &s.level, sizeof s.level) ||
        !copy(at(s.voxels, o.world_store), &s.store, sizeof s.store) ||
        !copy(at(s.voxels, o.world_resources), &s.resources, sizeof s.resources))
        return Held::Unreadable;
    if (!s.store) return Held::NotReady;
    if (!copy(at(s.store, o.store_unavailable), &s.store_unavailable, sizeof s.store_unavailable)) return Held::Unreadable;
    return Held::Ok;
}
}  // namespace

bool same(const Snapshot& a, const Snapshot& b) noexcept {
    return std::memcmp(a.iface, b.iface, sizeof a.iface) == 0 && a.voxels == b.voxels &&
           a.initialized == b.initialized && std::memcmp(a.origin, b.origin, sizeof a.origin) == 0 &&
           a.level.tile_x == b.level.tile_x && a.level.tile_z == b.level.tile_z &&
           a.level.count_x == b.level.count_x && a.level.count_z == b.level.count_z &&
           a.level.allocator == b.level.allocator && a.level.handles == b.level.handles &&
           a.level.handle_count == b.level.handle_count && a.store == b.store && a.resources == b.resources &&
           a.store_unavailable == b.store_unavailable;
}

Held hold(const HoldInput& in, Snapshot& out) noexcept {
    // 1. The gate: the GameServer thread, its running Scene and the authority view still hold.
    KILN_HOLD_POINT(HoldPoint::Begin);
    if (!in.scope->validate()) return Held::Gate;
    // 2. Two snapshots, compared field by field: nothing of the grid may move between them.
    Snapshot first, second;
    if (const Held h = copy_snapshot(in, first); h != Held::Ok) return h;
    KILN_HOLD_POINT(HoldPoint::BetweenCopies);
    if (const Held h = copy_snapshot(in, second); h != Held::Ok) return h;
    if (!same(first, second)) return Held::Moved;
    // 3. The gate again, after the last copy.
    if (!in.scope->validate()) return Held::Gate;
    out = first;
    return Held::Ok;
}

bool ready(const Snapshot& s, const Offsets& o, const Published& p) noexcept {
    constexpr uint64_t maximum_coordinate = 0x7fffffffull - 7;  // read_registered_voxel_grid.cpp:16
    const Level& l = s.level;
    const uint64_t x = uint64_t{l.tile_x} * l.count_x, z = uint64_t{l.tile_z} * l.count_z;
    return s.iface[0] == p.wrapper && s.voxels == p.voxels && s.iface[1] == o.read && s.iface[3] == o.ready &&
           s.initialized != 0 && std::isfinite(s.origin[0]) && std::isfinite(s.origin[1]) && std::isfinite(s.origin[2]) &&
           l.tile_x >= 8 && l.tile_z >= 8 && l.tile_x % 8 == 0 && l.tile_z % 8 == 0 && l.count_x && l.count_z &&
           l.handles && l.handle_count == uint64_t{l.count_x} * l.count_z && x <= maximum_coordinate &&
           z <= maximum_coordinate && s.store && !s.store_unavailable && s.resources;
}

}  // namespace kb::tasks
