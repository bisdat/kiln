// SPDX-License-Identifier: MIT
// kiln.decisions: which world a hooked game program runs in, with nothing of Windows in it (tests/world_gate_test.cpp
// runs it on the host with made-up memory).
//
// A game client that hosts steps three worlds in one process: the server world (its own thread, workers), the client
// world and the prediction world (the main thread). Their entity ids overlap, and the prediction world runs the client
// world's program table, so the death and pickup programs Kiln hooks can run for any of them. A joined client has no
// server world at all. kiln.decisions' holds, metadata and epoch are the server world's: a hook that is not inside the
// server world's own invocation must pass straight to the game and touch none of it.
//
// The test is the one kiln.world's program wraps and tracked writes use (native/services/world/wraps.cpp kind_is,
// writes.cpp writes_update): a ProgramContext's qword 0 is the job J, J's qword 0 is the slice's entity storage, and
// the server world's storage is `server world + world.storage` with kind byte 0 (KILN_WORLD_SERVER) at
// `storage + storage.kind`. The server world is the core's (KilnApi.server_world(): the pointer the tick hook
// publishes at the world's first step, an atomic load, 0 when there is none). Nothing else is read: no thread or phase
// check stands in for it (the review of 2026-11-14: a CHUNKED program can be on the world thread inside Sim, and a
// client's main thread runs other worlds' programs).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

namespace kiln::decisions {

class WorldGate {
public:
    // The core constants world.storage and storage.kind (sigdb, both sides). Unknown (negative): nothing is ever the
    // server world's, and the producers stay out.
    void set_layout(int64_t world_storage, int64_t storage_kind) {
        world_storage_ = world_storage;
        storage_kind_ = storage_kind;
    }
    bool layout_known() const { return world_storage_ > 0 && storage_kind_ >= 0; }

    // Any thread (a worker inside the sim). `ctx`: the hooked program's ProgramContext; `server_world`: the core's
    // server world now. Reads ctx's qword 0 and the job's qword 0 (the program's own context), compares that storage
    // with the server world's, and only then reads the kind byte, which lies inside the server world's own storage.
    bool owns(const void* ctx, uintptr_t server_world) const {
        if (!ctx || !server_world || !layout_known()) return false;
        const uint8_t* job = nullptr;
        std::memcpy(&job, ctx, sizeof job);
        if (!job) return false;
        uintptr_t storage = 0;
        std::memcpy(&storage, job, sizeof storage);
        if (storage != server_world + static_cast<uintptr_t>(world_storage_)) return false;
        return *reinterpret_cast<const uint8_t*>(storage + static_cast<uintptr_t>(storage_kind_)) == 0;
    }

private:
    int64_t world_storage_ = -1, storage_kind_ = -1;
};

}  // namespace kiln::decisions
