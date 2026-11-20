// SPDX-License-Identifier: MIT
// kiln-emf compatibility: EMF mods whose on_tick is only meant for the server world.
//
// EMF runs a mod's on_tick after every simulation step of every world (server, prediction, client), each on the
// thread that owns that world (emf_api.h; upstream EMF does the same). In a game that hosts its own world the server
// world steps on its own thread and the client and prediction worlds on the main thread, so one mod's on_tick can run
// on two threads at once.
//
// Expeditions (GronGM, github.com/GronGM/enshrouded-expeditions) up to 1.2.x returns at once from on_tick for every
// world but the server's, and then clears the global world pointer its server-world work reads through
// (src/emf_host.cpp on_tick / on_tick_inner). A client or prediction tick that lands during a survivor search empties
// it: "No free survivors nearby" next to the survivors, and Send's "too far away" (Kiln inbox 35, confirmed by a
// diagnostic build on a Steam Deck). Its other worlds' ticks do nothing else, so kiln-emf calls its on_tick for the
// server world only. Versions after 1.2.90 (1.2.90 = Kiln's diagnostic build) get every world's tick again.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace kemf {

struct TickCompat {
    std::string_view id;
    std::array<int64_t, 3> max;  // the newest version it applies to (inclusive)
};

inline constexpr TickCompat kServerTicksOnly[] = {
    {"expeditions", {1, 2, 90}},
};

// True when the mod `id` at version `v` gets its on_tick for the server world only.
inline bool server_ticks_only(std::string_view id, const std::array<int64_t, 3>& v) {
    for (const TickCompat& c : kServerTicksOnly) {
        if (c.id != id) continue;
        for (size_t i = 0; i < 3; ++i) {
            if (v[i] != c.max[i]) return v[i] < c.max[i];
        }
        return true;
    }
    return false;
}

}  // namespace kemf
