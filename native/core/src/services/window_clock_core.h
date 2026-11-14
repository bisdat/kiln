// SPDX-License-Identifier: MIT
// kiln.window_clock: the state machine, without anything of the core around it (host-tested as it is:
// tests/window_clock_test.cpp). window_clock.cpp publishes it as a service; tick.cpp drives it.
//
// One writer at a time (the server world's thread; the destroy hook runs on that thread too), any number of readers.
// A mutex guards the state: readers are API calls in Pre, never hooks on a hot path.
#pragma once
#include <cstring>
#include <mutex>

#include "services/kiln_component_changes.h"
#include "services/kiln_decisions.h"
#include "services/kiln_window_clock.h"

namespace kiln::window_clock {

using WindowFn = void(KILN_CALL*)(uint64_t world_epoch, uint64_t step);

// How a window call is made: returns false when it faulted. The core passes a SEH-guarded caller.
using Caller = bool (*)(WindowFn fn, uint64_t world_epoch, uint64_t step);

inline bool call_plain(WindowFn fn, uint64_t world_epoch, uint64_t step) {
    fn(world_epoch, step);
    return true;
}

// The two window services, in the order of window_begin. A table without the member (an older, shorter table) or a
// service that faulted in a window call is left out.
struct Windows {
    WindowFn decisions_begin = nullptr, decisions_end = nullptr;
    WindowFn changes_begin = nullptr, changes_end = nullptr;

    bool any() const { return decisions_begin || decisions_end || changes_begin || changes_end; }

    static Windows from(const void* decisions, const void* changes) {
        Windows w;
        if (const auto* d = static_cast<const KilnDecisionsApiV1*>(decisions);
            d && d->header.size >= offsetof(KilnDecisionsApiV1, window_end) + sizeof(void*)) {
            w.decisions_begin = d->window_begin;
            w.decisions_end = d->window_end;
        }
        if (const auto* c = static_cast<const KilnComponentChangesApiV1*>(changes);
            c && c->header.size >= offsetof(KilnComponentChangesApiV1, window_end) + sizeof(void*)) {
            w.changes_begin = c->window_begin;
            w.changes_end = c->window_end;
        }
        return w;
    }
};

class Core {
public:
    void set_session(const uint8_t bytes[16]) {
        std::lock_guard lock(mutex_);
        std::memcpy(state_.process_session, bytes, 16);
    }

    KilnWindowStateV1 get() const {
        std::lock_guard lock(mutex_);
        KilnWindowStateV1 s = state_;
        s.size = sizeof s;
        return s;
    }

    // KILN_WORLD_STARTED of the server world: a new epoch. The step counter of the tick hook goes on (it is
    // process-wide); phase stays 0 until the first Pre.
    void world_started() {
        std::lock_guard lock(mutex_);
        ++state_.world_epoch;
        state_.phase = KILN_WINDOW_NONE;
        state_.world_thread = 0;
    }

    // Before the Pre subscribers.
    void pre(uint64_t step, uint32_t thread) {
        std::lock_guard lock(mutex_);
        state_.step = step;
        state_.world_thread = thread;
        state_.phase = KILN_WINDOW_PRE;
    }

    // After the last Pre subscriber, before the game's sim step. With `windows` (feature tick.wait_jobs): phase 2,
    // decisions then component_changes, then phase 3. Without: straight to phase 3, no call.
    // Returns a mask of the services that faulted (1 decisions, 2 component_changes).
    uint32_t begin(bool windows, const Windows& w, Caller call = &call_plain) {
        uint32_t faulted = 0;
        if (windows) {
            const auto [epoch, step] = set_phase(KILN_WINDOW_PREPARING);
            if (w.decisions_begin && !call(w.decisions_begin, epoch, step)) faulted |= 1;
            if (w.changes_begin && !call(w.changes_begin, epoch, step)) faulted |= 2;
        }
        set_phase(KILN_WINDOW_SIM);
        return faulted;
    }

    // After WaitWorldJobs, before the Post subscribers: phase 4, component_changes then decisions, then phase 5.
    // Call it only when the world's jobs were waited for.
    uint32_t end(const Windows& w, Caller call = &call_plain) {
        uint32_t faulted = 0;
        const auto [epoch, step] = set_phase(KILN_WINDOW_AFTER_JOBS);
        if (w.changes_end && !call(w.changes_end, epoch, step)) faulted |= 2;
        if (w.decisions_end && !call(w.decisions_end, epoch, step)) faulted |= 1;
        set_phase(KILN_WINDOW_POST);
        return faulted;
    }

    // KILN_WORLD_STOPPING of the server world, before its subscribers.
    void stopping() { set_phase(KILN_WINDOW_STOPPING); }

    // After the last KILN_WORLD_STOPPING subscriber: no server world.
    void stopped() {
        std::lock_guard lock(mutex_);
        state_.phase = KILN_WINDOW_NONE;
        state_.world_thread = 0;
    }

private:
    struct At {
        uint64_t epoch, step;
    };
    At set_phase(uint32_t phase) {
        std::lock_guard lock(mutex_);
        state_.phase = phase;
        return {state_.world_epoch, state_.step};
    }

    mutable std::mutex mutex_;
    KilnWindowStateV1 state_{};
};

}  // namespace kiln::window_clock
