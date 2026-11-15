// SPDX-License-Identifier: MIT
// kiln.spawn's thread gate for the calls that create or destroy entities (since 0.2.0-dev.29): spawn, spawn_item,
// destroy and cleanup. They run on the tick thread in Pre or Post (kiln_spawn.h). The tick thread is also where the
// world runs a slice inline (a players mask of 0): a wrap stage, a Kiln program's update or a kiln.types system
// callback there passes the tick-thread check while the world is in the middle of its step. Mid-step the entity map
// must not change under the step's programs (the game defers their structural changes to the step's end), so those
// calls refuse with KILN_ERR_WRONG_THREAD while kiln.world's in_program answers 1, and kiln.log says to post them
// (slice.Post in a wrap stage, Scheduler.Post) so they run after the step. Reads (entity_info, find, refresh,
// templates, tracking) keep their dev.28 behaviour. A kiln.world without in_program (older, or none) answers "not
// inside": the calls behave as before.
#pragma once
#include <cstddef>

#include "kiln_api.h"
#include "services/kiln_world.h"

namespace kspawn {

enum class Gate { Ok, OffTick, MidStep };

inline bool world_mid_step(const KilnWorldApi* w) {
    return w && w->header.size >= offsetof(KilnWorldApi, in_program) + sizeof w->in_program && w->in_program &&
           w->in_program() != 0;
}

// on_tick: the core's on_tick_thread(); w: kiln.world's table (may be null).
inline Gate structural_gate(bool on_tick, const KilnWorldApi* w) {
    if (!on_tick) return Gate::OffTick;
    return world_mid_step(w) ? Gate::MidStep : Gate::Ok;
}

inline constexpr char kMidStepText[] =
    "called inside the world's step (a wrap stage, a Kiln program or a system callback), where the entity map must "
    "not change; post it (slice.Post in a wrap stage, or Scheduler.Post) so it runs after the step";

}  // namespace kspawn
