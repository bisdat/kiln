// SPDX-License-Identifier: MIT
// kiln.build's task-thread voxel members: everything before the gate and after its release; see voxel_tasks.h. The hold
// itself is voxel_tasks_hold.cpp.
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer): modules/game-runtime/native/voxels/
// read_registered_voxel_grid.cpp:394-435 (GetSpecForSession: the arguments, the session, the gate, the session again
// after the release, the cell size and the maximum per axis); the differences are voxel_tasks.h's.
#include "voxel_tasks.h"

#include <cstring>

#include "kiln_engine_view.h"

namespace kb::tasks {

namespace {
using kiln::engine::EcsGateScope;
using kiln::engine::EcsView;
using kiln::engine::GateAdmission;
using kiln::engine::Reason;
using kiln::engine::Role;

// The gate's (and the engine view's) refusals: busy (a later attempt can win) or unavailable (not from here, or not now).
KilnResult gate_result(Reason r) {
    switch (r) {
        case Reason::InFrame:
        case Reason::KilnHolderBusy:
        case Reason::KilnBusyLatched:
        case Reason::EcsSceneFrameBusy:
        case Reason::EcsThreadRegistryBusy:
        case Reason::EcsEngineOwnerChangedDuringRead:
        case Reason::EcsServerThreadChangedDuringRead:
        case Reason::EcsActiveSceneListChanged:
        case Reason::EcsReadySceneContextChanged:
        case Reason::EcsActiveSceneIsTransitioning:
        case Reason::EcsWriteOwnerChangedOrNotAuthoritative:
            return KILN_ECS_ERR_BUSY;
        default:
            return KILN_ERR_UNAVAILABLE;
    }
}
bool has_gate(const KilnEngineNativeApi* e) {
    return e && e->header.size >= offsetof(KilnEngineNativeApi, ecs_gate_release) + sizeof(void*) && e->copy &&
           e->ecs_view && e->ecs_gate_acquire && e->ecs_gate_validate && e->ecs_gate_release;
}
}  // namespace

Grid& grid() {
    static Grid g;
    return g;
}

void Grid::configure(const Source& source, const Offsets& offsets, const char* off) {
    std::lock_guard lock(mu_);
    src_ = source;
    off_ = offsets;
    if (!off && !has_gate(source.engine)) off = "kiln.internal.engine has no write gate here";
    if (!off && !source.server) off = "no server world facts";
    if (!off && (offsets.grid_iface < 0 || offsets.wrapper_world < 0 || offsets.world_init < 0 ||
                 offsets.world_origin < 0 || offsets.world_levels < 0 || offsets.level < 0 || offsets.world_store < 0 ||
                 offsets.store_unavailable < 0 || offsets.world_resources < 0 || !offsets.read || !offsets.ready ||
                 !(offsets.cell > 0)))
        off = "kiln.build's voxel layout is incomplete on this build";
    off_reason_ = off ? off : "";
    pub_ = Published{};
}

bool Grid::available() const {
    std::lock_guard lock(mu_);
    return off_reason_.empty();
}

std::string Grid::unavailable_reason() const {
    std::lock_guard lock(mu_);
    return off_reason_;
}

void Grid::publish(const Published& p) {
    std::lock_guard lock(mu_);
    pub_ = p;
}

void Grid::clear() {
    std::lock_guard lock(mu_);
    pub_ = Published{};
}

KilnResult Grid::grid_for_session(KilnOwner owner, uint64_t expected_session, KilnStr id, KilnBuildGridSpec* out) const {
    if (!out || out->size < sizeof(KilnBuildGridSpec) || !owner || !expected_session) return KILN_ERR_INVALID_ARGUMENT;
    if (!id.ptr || id.len == 0 || id.len >= sizeof out->id || std::memchr(id.ptr, 0, id.len))
        return KILN_ERR_INVALID_ARGUMENT;
    if (id.len != 5 || std::memcmp(id.ptr, "voxel", 5) != 0) return KILN_ERR_NOT_FOUND;
    // Everything the call reads with, copied before the gate (the mutex is never held across it).
    Source src;
    Offsets off;
    Published pub;
    {
        std::lock_guard lock(mu_);
        if (!off_reason_.empty()) return KILN_ERR_UNAVAILABLE;
        src = src_;
        off = off_;
        pub = pub_;
    }
    // The session: the live server world's, the caller's, and the one the tick found the voxel world in.
    uintptr_t world = 0;
    uint64_t session = 0;
    if (!src.server(src.user, &world, &session)) return KILN_ECS_ERR_BUSY;
    if (!world || !session) return KILN_ERR_UNAVAILABLE;
    if (session != expected_session) return KILN_ECS_ERR_STALE;
    if (pub.session != session || pub.world != world || !pub.grid) return KILN_ECS_ERR_BUSY;
    // The gate's view: the local authority's (a client joined to another machine's world has none).
    EcsView view{};
    uint32_t reason = 0;
    if (src.engine->ecs_view(0, &view, &reason) != KILN_OK) return gate_result(static_cast<Reason>(reason));
    if (view.role != Role::LocalAuthority) return KILN_ERR_UNAVAILABLE;
    const uint64_t step = src.server_step ? src.server_step(src.user) : 0;
    Snapshot snap;
    Held held = Held::Gate;
    {
        // The caller's owner: a busy refusal latches that owner alone for the rest of the step (the dialect owner the
        // core designates, kiln mode's interpreter, never), never every caller of this service.
        EcsGateScope scope(*src.engine, view, owner, step ? step : 1, GateAdmission::KilnMod);
        if (!scope.held()) return gate_result(scope.reason());
        HoldInput in;
        in.scope = &scope;
        in.copy = src.engine->copy;
        in.offsets = &off;
        in.grid = pub.grid;
        held = hold(in, snap);
        scope.release();
    }
    switch (held) {
        case Held::Ok: break;
        case Held::Unreadable: return KILN_ECS_ERR_UNREADABLE;
        default: return KILN_ECS_ERR_BUSY;  // the gate moved, the two copies differ, or the world is not ready yet
    }
    if (!ready(snap, off, pub)) return KILN_ECS_ERR_BUSY;
    // The session again, after the release: a world that ended meanwhile publishes nothing.
    uintptr_t world_after = 0;
    uint64_t session_after = 0;
    if (!src.server(src.user, &world_after, &session_after) || session_after != expected_session || world_after != world)
        return KILN_ECS_ERR_STALE;
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    std::memcpy(out->id, "voxel", sizeof "voxel");
    for (int axis = 0; axis < 3; ++axis) {
        out->origin[axis] = snap.origin[axis];
        out->cell_size[axis] = off.cell;
    }
    out->maximum[0] = uint64_t{snap.level.tile_x} * snap.level.count_x;
    out->maximum[1] = 0x7fffffffull - 7;
    out->maximum[2] = uint64_t{snap.level.tile_z} * snap.level.count_z;
    return KILN_OK;
}

}  // namespace kb::tasks
