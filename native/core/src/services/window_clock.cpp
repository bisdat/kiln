// SPDX-License-Identifier: MIT
// kiln.window_clock v1 (abi/services/kiln_window_clock.h): which part of the server world's step runs right now, the
// world epoch and the process session. The tick hook drives it (tick.cpp) and, through it, calls the two window
// services (kiln.decisions, kiln.component_changes) after the last Pre subscriber and after WaitWorldJobs.
#include "window_clock.h"

#include <windows.h>

#include <bcrypt.h>

#include <atomic>

#include "../api.h"
#include "../common.h"
#include "../guard.h"
#include "../log.h"
#include "../registry.h"
#include "service.h"
#include "window_clock_core.h"

namespace kiln::window_clock {
namespace {

Core g_core;
std::atomic<bool> g_windows{false};
// Written on the world thread only (pre), read there.
Windows g_tables;
uint64_t g_reg_version = ~0ull;
bool g_fresh = true;
// A window service that faulted in a window call gets no further one in this process (1 decisions, 2 changes).
uint32_t g_faulted = 0;
std::atomic<bool> g_any{false};

struct Call {
    WindowFn fn;
    uint64_t epoch, step;
};
void KILN_CALL run_call(void* user) {
    const auto* c = static_cast<const Call*>(user);
    c->fn(c->epoch, c->step);
}
bool call_guarded(WindowFn fn, uint64_t epoch, uint64_t step) {
    Call c{fn, epoch, step};
    unsigned long code = 0;
    if (guard::task(&run_call, &c, &code)) return true;
    KLOG_E("window clock: a window call faulted (0x%08lx)", code);
    return false;
}

void note_faults(uint32_t mask) {
    if (!mask) return;
    if ((mask & 1) && !(g_faulted & 1)) KLOG_E("window clock: kiln.decisions faulted in a window call; it gets no further one");
    if ((mask & 2) && !(g_faulted & 2))
        KLOG_E("window clock: kiln.component_changes faulted in a window call; it gets no further one");
    g_faulted |= mask;
    g_reg_version = ~0ull;  // rebuild the table set without it
}

void refresh_tables() {
    const uint64_t v = reg::version();
    if (!g_fresh && v == g_reg_version) return;
    g_fresh = false;
    g_reg_version = v;
    const KilnApi* a = api();
    const void* d = (g_faulted & 1) ? nullptr : a->service(kstr(KILN_DECISIONS_SERVICE), KILN_DECISIONS_VERSION);
    const void* c =
        (g_faulted & 2) ? nullptr : a->service(kstr(KILN_COMPONENT_CHANGES_SERVICE), KILN_COMPONENT_CHANGES_VERSION);
    g_tables = Windows::from(d, c);
    g_any.store(g_tables.any(), std::memory_order_relaxed);
}

KilnResult KILN_CALL clock_get(KilnWindowStateV1* out) {
    if (!out) return KILN_ERR_INVALID_ARGUMENT;
    *out = g_core.get();
    return KILN_OK;
}

const KilnWindowClockApiV1 kTable = {{sizeof(KilnWindowClockApiV1), KILN_WINDOW_CLOCK_VERSION}, &clock_get};

bool init() {
    uint8_t session[16] = {};
    if (BCryptGenRandom(nullptr, session, sizeof session, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        KLOG_E("window clock: no random bytes for the process session; the clock is unavailable");
        return false;
    }
    g_core.set_session(session);
    return true;
}

}  // namespace

KILN_SERVICE(window_clock, KILN_WINDOW_CLOCK_SERVICE, KILN_WINDOW_CLOCK_VERSION, &kTable, &init);

void enable_windows(bool on) { g_windows.store(on); }
bool wants_end() { return g_windows.load(std::memory_order_relaxed) && g_any.load(std::memory_order_relaxed); }

void world_started() {
    g_core.world_started();
    g_fresh = true;  // services published since the last world are found at this world's first Pre
}

void pre(uint64_t step) {
    refresh_tables();
    g_core.pre(step, GetCurrentThreadId());
}

void begin() { note_faults(g_core.begin(g_windows.load(std::memory_order_relaxed), g_tables, &call_guarded)); }
void end() { note_faults(g_core.end(g_tables, &call_guarded)); }
void stopping() { g_core.stopping(); }
void stopped() { g_core.stopped(); }

}  // namespace kiln::window_clock
