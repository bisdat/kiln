// SPDX-License-Identifier: MIT
// What the core guarantees about a world's jobs when it dispatches that world's KILN_WORLD_STOPPING (since 0.2.0-dev.29),
// per world kind: the capability rows world.stop_drain.server / .client / .prediction (services/caps.cpp) and the
// wait hk_world_destroy makes (tick.cpp).
//
// The game's own drains (static, both builds; out/06a-program-wraps/client-static/r8): StepPrograms waits for the world's
// whole task queue (WaitTaskGroup on world+world.jobs) before it returns whenever it ran slices as tasks (server
// S 0x64a516, client C 0x9ae686); serial slices and RunInitPrograms run inline. WorldDestroy itself does not wait: it
// hands the queue to the release (S 0x5d6510 / C 0x8eb7d0), which only checks that it is idle and logs "[task] TaskQueue
// '%s' is not idle during destroy!" before freeing it. Each world has its own queue (made in WorldCreate, C 0x983a53),
// and is destroyed on the thread that steps it (the GameServer thread, the main thread for the client world). So no
// program slice of a world runs when its destroy begins; other jobs of the world's queue can, on the server (posted
// without a wait and drained by WaitWorldJobs at the end of the GameServer update). The core calls WaitWorldJobs (the
// game's function: mov rcx,[world+world.jobs]; jmp WaitTaskGroup) for the server and the client world before their
// STOPPING. The prediction world has no queue of its own (its StepPrograms is serial) and stops with the client world,
// after that wait.
#pragma once
#include <cstdint>

#include "kiln_api.h"

namespace kiln::tick {

enum class StopDrain : uint8_t {
    None,     // no STOPPING is seen: the tick or the world-destroy hook is not installed, or faulted
    Bounded,  // STOPPING comes without the engine's wait (tick.wait_jobs did not resolve): a subscriber's own wait only
    Engine,   // the world's jobs are drained by the engine's own wait before STOPPING
    NoWorld,  // no world of this kind in this process (a client or prediction world in a dedicated server)
};

// side: KILN_SIDE_*; installed/lifecycle: the tick and world-destroy hooks work; wait_jobs: WaitWorldJobs resolved.
inline StopDrain stop_drain_of(uint32_t kind, uint32_t side, bool installed, bool lifecycle, bool wait_jobs) {
    if (kind >= KILN_WORLD_KINDS) return StopDrain::NoWorld;
    if (side != KILN_SIDE_CLIENT && kind != KILN_WORLD_SERVER) return StopDrain::NoWorld;
    if (!installed || !lifecycle) return StopDrain::None;
    return wait_jobs ? StopDrain::Engine : StopDrain::Bounded;
}

// The world kinds whose own object hk_world_destroy hands to WaitWorldJobs before STOPPING (the prediction world is not
// a world object: it is drained with the client world).
inline bool waits_before_stop(uint32_t kind) { return kind == KILN_WORLD_SERVER || kind == KILN_WORLD_CLIENT; }

}  // namespace kiln::tick
