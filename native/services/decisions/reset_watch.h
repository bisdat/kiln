// SPDX-License-Identifier: MIT
// kiln.decisions: the invalidation after a rollback, with nothing of the game in it (tests/reset_test.cpp).
//
// Chapter 07 §4.1: an entity-counter regression (the world's entity ids went back: a rollback, a reload of the same
// world) clears the metadata and invalidates what is pending "even if the world pointer did not change". Such a world
// can bring back an entity with the same id and the same SpawnTime, and then a pending decision, a death hold, a pickup
// suppression or a permit would apply to what is, for the game, another past. kiln.items already watches the counter
// (native/services/items/instances.cpp instances_pre, StampTable::observe_counter) and counts every time it forgets its
// item stamps (`KilnItemsInstancesStatus.resets`: the counter went down, or a world closed or started). That count is
// the reset generation kiln.decisions follows: read at every window_begin (after the Pre in which kiln.items looked at
// the counter), a change since the last window invalidates the metadata, every open request (StaleEntity) and both
// producers' state together, before anything is sampled or held again in that window. Holds end fail open: a death
// hold that goes lets the game go on, a suppression that goes lets the pickup run.
#pragma once
#include <cstdint>

#include "metadata.h"
#include "queue.h"

namespace kiln::decisions {

class ResetWatch {
public:
    static constexpr uint64_t kUnknown = UINT64_MAX;

    // window_begin, world thread. `generation`: kiln.items' reset count now, kUnknown when it cannot be read (no
    // kiln.items, item instances off). True once for every change after the first value seen.
    bool changed(uint64_t generation) {
        if (generation == kUnknown) return false;
        if (!seen_) {
            seen_ = true;
            last_ = generation;
            return false;
        }
        if (generation == last_) return false;
        last_ = generation;
        ++invalidations_;
        return true;
    }
    uint64_t invalidations() const { return invalidations_; }

private:
    bool seen_ = false;
    uint64_t last_ = 0, invalidations_ = 0;
};

// The invalidation itself (world thread, no world job live). H and X: the death holds and the pickup exposures (or
// adapters with clear()). Returns how many open requests it ended.
template <class H, class X>
uint32_t invalidate_after_reset(Broker& broker, Metadata& metadata, H&& holds, X&& exposures, uint64_t step) {
    const uint32_t open = broker.active_count();
    broker.release_open(KILN_DECISION_STALE_ENTITY, "the world's entity ids went back (a rollback): what was held belongs to another past; released",
                        step);
    holds.clear();
    exposures.clear();
    metadata.clear();
    return open;
}

}  // namespace kiln::decisions
