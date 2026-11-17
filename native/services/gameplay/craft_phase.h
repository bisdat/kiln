// SPDX-License-Identifier: MIT
// Which calls of the game's craft core (CraftRecipe, gameplay/CraftRecipe) kiln.gameplay reports as CRAFTED
// (ItemCraftedEvent). Pure: no game memory beyond the context bytes it is given; host test tests/craft_phase_test.cpp.
//
// CraftRecipe(CraftContext* ctx, RecipeInfo*): ctx +0 operator, +4 station, +8 workshop, a flags byte at
// +flags_offset (0x10 on 26430253 / 26430625, captured as gameplay/craft.flags_frame - gameplay/craft.ctx_frame).
// A player's or a request's craft runs the whole recipe in one call (flags 0). An automated station (factory_station:
// operator == station, not a player) runs it in two calls per cycle: at the start with flag 0x08 (the inputs only) and
// at the end with flag 0x10 (gameplay/craft.outputs_only: the outputs only, payment skipped). docs/re/stations.md §4.
#pragma once
#include <cstdint>
#include <cstring>

namespace kg {

struct CraftPhase {
    int64_t flags_offset = -1;  // byte offset of the flags in the context; -1: not known on this build
    uint32_t outputs_only = 0;  // the flag of a factory cycle's end (0x10)
    bool known() const { return flags_offset >= 0 && flags_offset < 0x100 && outputs_only != 0; }
};

// An automated station's call: the station runs its own recipe (operator == station) and is no player (1..1023).
inline bool craft_is_factory(const uint32_t* ctx) {
    return ctx && ctx[0] == ctx[1] && !(ctx[0] >= 1 && ctx[0] <= 1023);
}

// Whether to report this call. Players' and requests' crafts: every outcome. Automated stations retry every step while
// materials are missing: their successes only, and of a cycle's two calls only the end (its outputs), so one cycle is
// one report. Where the phase is not known (an older sigdb) a factory's success is reported at both calls, as before.
inline bool craft_reported(const uint32_t* ctx, int32_t rc, const CraftPhase& phase) {
    if (!ctx) return false;
    if (!craft_is_factory(ctx)) return true;
    if (rc != 0) return false;
    if (!phase.known()) return true;
    uint8_t flags = 0;
    std::memcpy(&flags, reinterpret_cast<const uint8_t*>(ctx) + phase.flags_offset, 1);
    return (flags & phase.outputs_only) != 0;
}

}  // namespace kg
