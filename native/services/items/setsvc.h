// SPDX-License-Identifier: MIT
// kiln.items v6: what sets.cpp (the store, the lookups, the capability's state) gives the executor (setexec.cpp).
// Internal to the service.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "setsnap.h"
#include "services/kiln_decisions.h"
#include "services/kiln_window_clock.h"

namespace kitems {

// kiln.window_clock and kiln.decisions, looked up at the first v6 call; null when not here.
const KilnWindowClockApiV1* sets_clock();
const KilnDecisionsApiV1* sets_decisions();
// This build's component layouts (kiln.types, checked once); null with the reason when they do not match.
const sets::SetLayout* sets_layout(char* why, size_t cap);

// Whether sets can run now; the reason when not. Any thread.
bool sets_available(char* why, size_t cap);
// Switches sets off until the server restarts (an invariant failure, a fault inside the transaction). Logged once.
void sets_break(const char* why);
bool sets_broken();

struct SetCounters {
    std::atomic<uint32_t> pending{0};
    std::atomic<uint64_t> done{0}, refused{0}, invariants{0};
};
SetCounters& set_counters();

// The store under its lock (sets.cpp). take moves a snapshot out (an accepted set); peek_model copies one (a plan).
sets::SnapStore::Find sets_take(uint64_t token, KilnOwner owner, uint64_t epoch, uint64_t now, sets::SnapEntry* out);
bool sets_peek_model(uint64_t token, KilnOwner owner, uint64_t epoch, uint64_t now, sets::SnapStore::Find* f, sets::SetModel* model,
                     sets::SnapPrefix* prefix, uint64_t* captured_step);

}  // namespace kitems
