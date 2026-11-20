// SPDX-License-Identifier: MIT
// kiln.decisions: the coordinated shutdown after a fault, with nothing of the game or of Windows in it
// (tests/shutdown_test.cpp).
//
// A window call that faults is never called again (the core's window clock leaves the service out), and a hook that
// faults is never trusted again. Either way the deadlines in queue.h would stop advancing (they move at the window
// calls) and a hold could outlive its fail-open deadline. So a fault shuts the producers down together:
//   1. stop(): at once and lock free, from any thread (a hook's fault handler on a worker): holding() is false, and
//      both hooks pass every visit straight to the game before they read or lock anything;
//   2. finish(): on the world thread (the core's window_faulted, or the next window call or consumer call after a
//      hook's fault): every open request ends CAPABILITY_RELEASED (fail open) and the hold and exposure tables are
//      emptied, without reading the world; the rows are lowered with the reason; the caller logs it once.
// A fault can leave one of the service's locks held (an SEH unwind runs no destructor). finish() only ever tries those
// locks: if the broker's is held, its requests are not ended here and broker_ok() is false, so the service never
// calls the broker again (no deadlock); the hooks pass regardless.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "queue.h"
#include "services/kiln_caps.h"

namespace kiln::decisions {

// Where finish() lowers a row (the service: kiln.caps; a test: a list).
using RowSink = void (*)(const char* row, uint32_t reason, const char* why, void* user);

// The rows a shutdown lowers: both producers, the equipment verdict (it needs the broker), the guard seam and the
// service itself.
inline constexpr const char* kShutdownRows[] = {"events.death", "events.pickup", "events.equip", "decisions.native_guards",
                                                KILN_DECISIONS_SERVICE};

class Shutdown {
public:
    // Any thread, lock free: false once stop() ran. The hooks ask it first.
    bool holding() const { return !stopped_.load(std::memory_order_acquire); }
    // Any thread: stop holding now. The first call's `why` is kept; finish() does the rest.
    void stop(const char* why) {
        if (stopped_.exchange(true, std::memory_order_acq_rel)) return;
        std::snprintf(why_, sizeof why_, "%s", why ? why : "a fault");
        reason_ready_.store(true, std::memory_order_release);
        pending_.store(true, std::memory_order_release);
    }
    // A finish() is due (a stop() that was not finished yet). Any thread.
    bool pending() const { return pending_.load(std::memory_order_acquire); }
    // Whether the broker may still be called (its lock was free at finish()). Any thread.
    bool broker_ok() const { return broker_ok_.load(std::memory_order_acquire); }
    const char* why() const { return reason_ready_.load(std::memory_order_acquire) ? why_ : "a fault is being reported"; }

    struct Finished {
        bool released = false;  // the open requests were ended (else the broker's lock was held)
        bool cleared = false;   // the hold and exposure tables were emptied
    };

    // World thread. Carries out a pending stop once; false when none is pending. H and X have try_clear() (the death
    // holds and the pickup exposures, or adapters to them).
    template <class H, class X>
    bool finish(Broker& broker, H&& holds, X&& exposures, uint64_t step, RowSink sink, void* user, Finished* out = nullptr) {
        if (!pending_.exchange(false, std::memory_order_acq_rel)) return false;
        Finished f;
        char reason[192];
        std::snprintf(reason, sizeof reason, "kiln.decisions shut down: %s; the action was released", why_);
        broker_ok_.store(false, std::memory_order_release);
        f.released = broker.try_release_all(KILN_DECISION_CAPABILITY_RELEASED, reason, step);
        if (f.released) broker_ok_.store(true, std::memory_order_release);
        const bool h = holds.try_clear(), x = exposures.try_clear();
        f.cleared = h && x;
        for (const char* row : kShutdownRows)
            if (sink) sink(row, KILN_CAP_REASON_FAULTED, why_, user);
        if (out) *out = f;
        return true;
    }

private:
    std::atomic<bool> stopped_{false}, pending_{false}, broker_ok_{true}, reason_ready_{false};
    char why_[128] = {};
};

}  // namespace kiln::decisions
