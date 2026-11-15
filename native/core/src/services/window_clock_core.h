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
using WantedFn = uint32_t(KILN_CALL*)(void);       // kiln.decisions' window_wanted (since 0.2.0-dev.29)
using FaultedFn = void(KILN_CALL*)(uint32_t code);  // kiln.decisions' window_faulted (since 0.2.0-dev.29)

// How a window call is made: returns false when it faulted. The core passes a SEH-guarded caller.
using Caller = bool (*)(WindowFn fn, uint64_t world_epoch, uint64_t step);
// How window_wanted is asked: false when it faulted (then *out is 0). The core passes a SEH-guarded asker.
using Asker = bool (*)(WantedFn fn, uint32_t* out);

inline bool call_plain(WindowFn fn, uint64_t world_epoch, uint64_t step) {
    fn(world_epoch, step);
    return true;
}
inline bool ask_plain(WantedFn fn, uint32_t* out) {
    *out = fn();
    return true;
}
// How window_faulted is called (the core guards it like a window call).
using Teller = bool (*)(FaultedFn fn, uint32_t code);
inline bool tell_plain(FaultedFn fn, uint32_t code) {
    fn(code);
    return true;
}

// The two window services, in the order of window_begin. A table without the member (an older, shorter table) or a
// service that faulted in a window call is left out. kiln.decisions may say a step does not need it (window_wanted):
// then it gets neither call in that step; a table without window_wanted is called every step.
struct Windows {
    WindowFn decisions_begin = nullptr, decisions_end = nullptr;
    WantedFn decisions_wanted = nullptr;
    FaultedFn decisions_faulted = nullptr;
    WindowFn changes_begin = nullptr, changes_end = nullptr;

    bool any() const { return decisions_begin || decisions_end || changes_begin || changes_end; }

    static Windows from(const void* decisions, const void* changes) {
        Windows w;
        if (const auto* d = static_cast<const KilnDecisionsApiV1*>(decisions);
            d && d->header.size >= offsetof(KilnDecisionsApiV1, window_end) + sizeof(void*)) {
            w.decisions_begin = d->window_begin;
            w.decisions_end = d->window_end;
            if (d->header.size >= offsetof(KilnDecisionsApiV1, window_wanted) + sizeof(void*)) w.decisions_wanted = d->window_wanted;
            if (d->header.size >= offsetof(KilnDecisionsApiV1, window_faulted) + sizeof(void*)) w.decisions_faulted = d->window_faulted;
        }
        if (const auto* c = static_cast<const KilnComponentChangesApiV1*>(changes);
            c && c->header.size >= offsetof(KilnComponentChangesApiV1, window_end) + sizeof(void*)) {
            w.changes_begin = c->window_begin;
            w.changes_end = c->window_end;
        }
        return w;
    }
};

// A fault mask of begin()/end() names kiln.decisions: tell it (window_faulted, when its table has the member) before
// the core leaves it out, so that it stops holding and ends what it holds instead of waiting for window calls that will
// not come. `already` is the mask of services told before (each is told once). Returns the services told now.
inline uint32_t tell_faulted(uint32_t mask, uint32_t already, const Windows& w, uint32_t code, Teller tell = &tell_plain) {
    if (!(mask & 1) || (already & 1) || !w.decisions_faulted) return 0;
    tell(w.decisions_faulted, code);
    return 1;
}

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
    // decisions (when it wants this step) then component_changes, then phase 3. Without: straight to phase 3, no call.
    // Returns a mask of the services that faulted (1 decisions, 2 component_changes); begun() says which were called.
    uint32_t begin(bool windows, const Windows& w, Caller call = &call_plain, Asker ask = &ask_plain) {
        uint32_t faulted = 0;
        begun_ = 0;
        if (windows) {
            const auto [epoch, step] = set_phase(KILN_WINDOW_PREPARING);
            if (w.decisions_begin) {
                uint32_t want = 1;
                if (w.decisions_wanted && !ask(w.decisions_wanted, &want)) faulted |= 1, want = 0;
                if (want) {
                    if (call(w.decisions_begin, epoch, step)) begun_ |= 1;
                    else faulted |= 1;
                }
            }
            if (w.changes_begin) {
                if (call(w.changes_begin, epoch, step)) begun_ |= 2;
                else faulted |= 2;
            }
        }
        set_phase(KILN_WINDOW_SIM);
        return faulted;
    }

    // The window services begin() called in this step without a fault (1 decisions, 2 component_changes): those
    // need end() after the world's jobs, also with no Post subscriber. World thread.
    uint32_t begun() const { return begun_; }

    // After WaitWorldJobs, before the Post subscribers: phase 4, component_changes then decisions (each only when it
    // was begun in this step), then phase 5. Call it only when the world's jobs were waited for.
    uint32_t end(const Windows& w, Caller call = &call_plain) {
        uint32_t faulted = 0;
        const auto [epoch, step] = set_phase(KILN_WINDOW_AFTER_JOBS);
        if ((begun_ & 2) && w.changes_end && !call(w.changes_end, epoch, step)) faulted |= 2;
        if ((begun_ & 1) && w.decisions_end && !call(w.decisions_end, epoch, step)) faulted |= 1;
        begun_ = 0;
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
    uint32_t begun_ = 0;  // world thread only (begin/end)
};

}  // namespace kiln::window_clock
