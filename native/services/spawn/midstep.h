// SPDX-License-Identifier: MIT
// kiln.spawn's thread gate for the calls that create or destroy entities (since 0.2.0-dev.29): spawn, spawn_item,
// destroy and cleanup. They run on the tick thread in Pre or Post (kiln_spawn.h). The tick thread is also where the
// world runs a slice inline (a players mask of 0), so code inside the world's step can pass the tick-thread check:
//   - a program wrap's stage, or a Kiln ECS program's callback (server, client or prediction world): refused with
//     KILN_ERR_WRONG_THREAD (kiln.world's in_program answers 1 there), and kiln.log says to post the call (slice.Post in
//     a wrap stage, Scheduler.Post). These paths are new in dev.29 or were refused already (managed: SliceGuard).
//   - a kiln.types system-hook callback (managed GameSystems.Hook, a native system_hook; kiln.types' in_system_callback
//     answers 1, and so does in_program): allowed as on dev.28 until the owner decides, with one warning per call kind
//     in kiln.log (Kiln.Spawn adds one per mod and call kind). EMF's hook_program refuses its own spawn and destroy there
//     itself (its world-callback rule), as before.
// Reads (entity_info, find, refresh, templates, tracking) keep their dev.28 behaviour. A kiln.world without in_program or
// a kiln.types without in_system_callback (older, or none) answers "not inside": the calls behave as before it existed.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "kiln_api.h"
#include "services/kiln_types.h"
#include "services/kiln_world.h"

namespace kspawn {

enum class Gate {
    Ok,            // Pre or Post on the tick thread
    OffTick,       // not the tick thread: refused, as before
    MidStep,       // a wrap stage or a Kiln program's callback: refused
    InSystemHook,  // a kiln.types system-hook callback: allowed (dev.28), warned
};

inline bool world_mid_step(const KilnWorldApi* w) {
    return w && w->header.size >= offsetof(KilnWorldApi, in_program) + sizeof w->in_program && w->in_program &&
           w->in_program() != 0;
}

inline bool in_system_hook(const KilnTypesApi* t) {
    return t && t->header.size >= offsetof(KilnTypesApi, in_system_callback) + sizeof t->in_system_callback &&
           t->in_system_callback && t->in_system_callback() != 0;
}

// on_tick: the core's on_tick_thread(); w, t: kiln.world's and kiln.types' tables (either may be null). A system-hook
// callback runs before or after a program's slice, never inside a stage or a Kiln program's update, so it is told first.
inline Gate structural_gate(bool on_tick, const KilnWorldApi* w, const KilnTypesApi* t) {
    if (!on_tick) return Gate::OffTick;
    if (in_system_hook(t)) return Gate::InSystemHook;
    return world_mid_step(w) ? Gate::MidStep : Gate::Ok;
}

// The call kinds, as bits for the once-per-kind warning.
enum : uint32_t { kKindSpawn = 1, kKindSpawnItem = 2, kKindDestroy = 4, kKindCleanup = 8 };

// True the first time a kind is seen (any thread).
struct OncePerKind {
    std::atomic<uint32_t> seen{0};
    bool first(uint32_t kind) { return !(seen.fetch_or(kind, std::memory_order_relaxed) & kind); }
};

inline constexpr char kMidStepText[] =
    "called inside the world's step (a program wrap's stage or a Kiln program), where the entity map must not change; "
    "post it (slice.Post in a wrap stage, or Scheduler.Post) so it runs after the step";
inline constexpr char kSystemHookText[] =
    "an entity created or destroyed in the middle of the world's step, from a kiln.types system-hook callback "
    "(GameSystems.Hook, a native system_hook): allowed as on 0.2.0-dev.28, but unsafe once the game runs its programs on "
    "worker threads (a second player in the world); post it with Scheduler.Post. Said once per call kind";

}  // namespace kspawn
