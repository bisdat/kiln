// SPDX-License-Identifier: MIT
// kiln.build's voxel members for task threads (kiln_build.h, "Voxels from task threads"): grid_for_session, the server
// world's voxel grid geometry from any thread but a world's own, under the engine's own write gate (kiln.internal.engine's
// EcsGateScope), between frames only. Portable: no Windows and no KilnApi here, so the host test runs this exact code over
// the real gate and a fake engine (tests/voxel_tasks_test.cpp); build.cpp wires it to the running game.
//
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer): modules/game-runtime/native/voxels/
// read_registered_voxel_grid.cpp:101-141 (read_snapshot: the interface and its wrapper, the VoxelWorld, its initialized
// flag, the level's tile sizes and counts, the store and its unavailable flag, the resources, the origin) and :394-435
// (GetSpecForSession: the session, the gate, two equal snapshots, the gate released before the caller's record is
// written, the session again; the cell size, the maximum per axis). Differences:
//   - Kiln finds ecs::VoxelGrid on the server tick (the world's singleton map is tick-only) and publishes it with the
//     session it was found in; the hold re-reads the grid's interface and checks it is still that one.
//   - The origin is the VoxelWorld's own field. The interface's origin slot is a copy of that field (docs/re/build.md
//     section 2), so no engine code is called at all.
//   - The VoxelWorld's reader word is not taken: the two copies are compared instead, under the exclusive Scene word.
//
// Inside the hold (voxel_tasks_hold.cpp): no wait, allocation, lock, log or KilnApi call, and a fixed number of guarded
// copies. Everything that may allocate or lock runs before the gate is taken or after it is released.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>

#include "kiln_engine_native.h"
#include "services/kiln_build.h"
#include "services/kiln_world.h"  // KILN_ECS_ERR_*

namespace kb::tasks {

// kiln.build's own layout of the voxel world (sigdb build.json), the fields grid_for_session reads.
struct Offsets {
    int64_t grid_iface = -1;     // ecs::VoxelGrid + this = its interface {wrapper, read, origin, ready, ...}
    int64_t wrapper_world = -1;  // wrapper + this = the VoxelWorld
    int64_t world_init = -1, world_origin = -1, world_levels = -1, level = -1, level_stride = 40;
    int64_t world_store = -1, store_unavailable = -1, world_resources = -1;
    std::uintptr_t read = 0, ready = 0;  // the interface's read and ready slots are these (build/VoxelGridRead, ...Ready)
    double cell = 0;                     // metres per cell: the base cell / 2^level
};

// The voxel world kiln.build found on the server tick for one server world session.
struct Published {
    uint64_t session = 0;  // game_world_session(KILN_WORLD_SERVER) when it was found
    std::uintptr_t world = 0, grid = 0, wrapper = 0, voxels = 0;
    bool operator==(const Published&) const = default;
};

struct Source {
    // kiln.internal.engine with its gate members (header.size covers ecs_gate_release), or NULL.
    const KilnEngineNativeApi* engine = nullptr;
    void* user = nullptr;
    // The live server world, sampled consistently: *world its pointer (0: none), *session game_world_session(SERVER).
    // False when the sample moved under the read.
    bool (*server)(void* user, std::uintptr_t* world, uint64_t* session) = nullptr;
    // The live server step; 0 when unknown.
    uint64_t (*server_step)(void* user) = nullptr;
};

// ---- the hold (voxel_tasks_hold.cpp) ----
struct Level {
    uint32_t tile_x = 0, tile_z = 0, count_x = 0, count_z = 0;
    uint64_t allocator = 0, handles = 0, handle_count = 0;
};
static_assert(sizeof(Level) == 40, "the engine's level record");
struct Snapshot {
    uint64_t iface[4] = {};  // wrapper, read, origin, ready
    uint64_t voxels = 0;     // wrapper + wrapper_world
    uint32_t initialized = 0;
    float origin[3] = {};
    Level level;
    uint64_t store = 0, resources = 0;
    uint8_t store_unavailable = 0;
};
bool same(const Snapshot& a, const Snapshot& b) noexcept;
enum class Held : uint32_t { Ok, Gate, Unreadable, Moved, NotReady };
enum class HoldPoint : uint32_t { Begin, BetweenCopies };
#ifdef KILN_BUILD_HOLD_TEST_POINTS
extern void (*g_hold_test_point)(HoldPoint);
#endif
struct HoldInput {
    const kiln::engine::EcsGateScope* scope = nullptr;  // held
    KilnResult(KILN_CALL* copy)(std::uintptr_t address, void* out, std::size_t bytes) = nullptr;  // guarded copy
    const Offsets* offsets = nullptr;
    std::uintptr_t grid = 0;  // the published ecs::VoxelGrid
};
// Two snapshots of the grid's voxel world, equal, with the gate still valid around them. Hold-safe.
Held hold(const HoldInput& in, Snapshot& out) noexcept;
// The snapshot is the published world's, its interface the one sigdb names, and every field checks out (pure).
bool ready(const Snapshot& s, const Offsets& o, const Published& p) noexcept;

// ---- the members ----
class Grid {
public:
    // From on_ready: what the members read with, and why they are off (NULL: on).
    void configure(const Source& source, const Offsets& offsets, const char* off);
    bool available() const;
    std::string unavailable_reason() const;
    // The server tick: the voxel world found for the live session.
    void publish(const Published& p);
    // The server world stops.
    void clear();
    // `owner`: the caller's, the gate's owner for this call (its busy latch is that owner's alone).
    KilnResult grid_for_session(KilnOwner owner, uint64_t expected_session, KilnStr id, KilnBuildGridSpec* out) const;

private:
    mutable std::mutex mu_;  // the configuration and the published world (never across a hold)
    Source src_;
    Offsets off_;
    Published pub_;
    std::string off_reason_ = "kiln.build has not started yet";
};
Grid& grid();

}  // namespace kb::tasks
