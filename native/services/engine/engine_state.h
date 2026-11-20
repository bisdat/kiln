// SPDX-License-Identifier: MIT
// kiln.engine's process-wide state shared by its translation units (not an ABI).
#pragma once
#include <atomic>
#include <cstdint>

#include "identity.h"
#include "kiln_api.h"

namespace kiln::engine {

struct Stats {
    std::atomic<std::uint64_t> attempts{0}, acquired{0}, busy_registry{0}, busy_scene{0}, refused_in_frame{0},
        refused_world_thread{0}, refused_other{0}, hold_ns_total{0}, hold_ns_max{0}, views{0}, view_failures{0};
};
Stats& stats();

// The loaded image's facts. facts_win.cpp on Windows; host tests link their own.
ImageFacts gather_facts(const KilnApi* api);

// The selected exact profile's layout (process lifetime), nullptr without one.
const Layout* selected_layout();

// Host tests only: forget the selection so kiln_plugin_main can run again in one process.
void reset_for_tests();

}  // namespace kiln::engine
