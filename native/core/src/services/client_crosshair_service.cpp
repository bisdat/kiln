// SPDX-License-Identifier: MIT
// kiln.client.crosshair v1 (abi/services/kiln_client_crosshair.h): the table, the live facts and the hook. The rules
// are client_crosshair.cpp's (host-tested); this file gives them the game: the client world's Pre (its session and step)
// and STOPPING, and the producer: one kiln.hooks mid link at the sigdb point client/CrosshairObserved (client_cursor,
// right after its call of the gameplay scene trace). The link goes in at the first get of the process (the lazy
// producer, client_crosshair.h), never at the start: a game without a mod that reads the crosshair never has it. A held
// client gate row (native/core/gates) would keep it out entirely.
#include <atomic>

#include "../api.h"
#include "../gates.h"
#include "../guard.h"
#include "../image.h"
#include "../log.h"
#include "../sigdb.h"
#include "../tick.h"
#include "client_crosshair.h"
#include "hooks_chain.h"
#include "hooks_internal.h"
#include "services/kiln_client_crosshair.h"
#include "service.h"

namespace kiln {
namespace {

constexpr const char* kCap = "client.crosshair";
constexpr const char* kFeature = "client.crosshair_trace";

uintptr_t g_site = 0;      // client/CrosshairObserved, when the producer can be installed
uint32_t g_hit = 0;        // client/crosshair.hit

bool guarded_read(void*, void* dst, uintptr_t src, size_t n) { return guard::copy(dst, src, n); }
const crosshair::Memory kMem{&guarded_read, nullptr};

// At client/CrosshairObserved: rbp = client_cursor's frame, the trace's result at [rbp + client/crosshair.hit]. Reads
// registers and that copy only; whatever else links the instruction sees the registers unchanged.
void KILN_CALL on_observed(KilnRegisters* r, void*) { crosshair::observe(r->rbp, kMem); }

uint32_t live_client_session() {
    KilnSessionInfo s{};
    s.size = sizeof s;
    if (tick::session(&s) != KILN_OK) return 0;
    const bool in_game = s.state == KILN_SESSION_HOST || s.state == KILN_SESSION_REMOTE;
    return in_game && s.worlds[KILN_WORLD_CLIENT] ? s.sessions[KILN_WORLD_CLIENT] : 0;
}

void KILN_CALL on_client_pre(const KilnTickInfo* info, void*) {
    crosshair::session_live(live_client_session(), info ? info->step : 0);
}

void KILN_CALL on_world(const KilnWorldEvent* ev, void*) {
    if (ev->kind == KILN_WORLD_CLIENT && ev->event == KILN_WORLD_STOPPING) crosshair::session_ended(ev->session);
}

// The lazy producer's installer: the first get's thread, under client_crosshair's install mutex. A checked
// installation: when it is the first link on the instruction, the detour is written with every other thread stopped and
// checked to stand outside the replaced bytes (a thread returning from the trace returns to the instruction's start,
// which is outside them), and refused, nothing written, when a thread could not be checked.
crosshair::Install install_producer(uint32_t* hit, const char** why) {
    hooks::chain::Order o;
    o.label = "kiln.client.crosshair/CrosshairObserved";
    KilnHandle h = 0;
    uint32_t unchecked = 0;
    const KilnResult rc = hooks::ensure_ready()
        ? hooks::chain::install_mid(0, g_site, o, &on_observed, nullptr, &h, &unchecked)
        : KILN_ERR_UNAVAILABLE;
    if (rc != KILN_OK && unchecked) {
        KLOG_W("client.crosshair: hooking the crosshair trace (client/CrosshairObserved) was refused: %u thread(s) could "
               "not be checked; a later read tries again",
               static_cast<unsigned>(unchecked));
        *why = "hooking the crosshair trace was refused on every attempt: threads that could not be checked (kiln.log "
               "says which)";
        return crosshair::Install::Again;
    }
    if (rc != KILN_OK) {
        KLOG_E("client.crosshair: hooking the crosshair trace (client/CrosshairObserved) at the first read failed (%d); "
               "the crosshair stays off until the game restarts", rc);
        *why = "hooking the crosshair trace failed at the first read (kiln.log says why)";
        return crosshair::Install::Failed;
    }
    *hit = g_hit;
    KLOG_I("client.crosshair: hooked at the first read (observation +0x%llx, result at rbp+0x%x)",
           static_cast<unsigned long long>(g_site - image().base), g_hit);
    return crosshair::Install::Bound;
}

// Once, from init() on the game client: readies the lazy producer (nothing is hooked here), or leaves it unbound with
// the reason kiln.caps shows.
void ready_producer() {
    uintptr_t site = 0;
    int64_t hit = 0;
    const char* why = nullptr;
    if (!hooks_enabled()) {
        why = "hooks are off (safe idle)";
    } else if (!sigdb::feature(kFeature) || !sigdb::symbol("client/CrosshairObserved", &site) || !site ||
               !sigdb::constant("client/crosshair.hit", &hit)) {
        crosshair::unbound_reason(nullptr);  // the default reason: no verified trace on this build
        KLOG_I("client.crosshair: unavailable: sigdb %s is not resolved on this game build", kFeature);
        return;
    } else if (hit <= 0 || hit >= static_cast<int64_t>(crosshair::kMaxFrameOffset)) {
        why = "sigdb client/crosshair.hit is outside client_cursor's frame";
    } else if (gates::is_held(kCap)) {
        why = "held by the client gate until its guided client checks pass (kiln.json \"preview\" lifts it)";
    }
    if (why) {
        crosshair::unbound_reason(why);
        KLOG_I("client.crosshair: not bound: %s", why);
        return;
    }
    g_site = site;
    g_hit = static_cast<uint32_t>(hit);
    crosshair::set_installer(&install_producer);
    KLOG_I("client.crosshair: ready on this build (observation +0x%llx); hooked at the first read",
           static_cast<unsigned long long>(site - image().base));
}

KilnResult KILN_CALL c_get(KilnCrosshairHit* out, uint32_t* out_reason) {
    return crosshair::get({image().side == KILN_SIDE_CLIENT, hooks_enabled(), live_client_session()}, out, out_reason);
}

const KilnClientCrosshairApi kTable = {
    {sizeof(KilnClientCrosshairApi), KILN_CLIENT_CROSSHAIR_VERSION},
    &c_get,
};

bool init() {
    if (!tick::installed()) return false;
    if (image().side != KILN_SIDE_CLIENT) return true;  // the table answers: get is unavailable on a dedicated server
    KilnHandle h = 0;
    tick::subscribe_world(0, &on_world, nullptr, &h);
    tick::subscribe_world_tick(0, KILN_WORLD_CLIENT, KILN_TICK_PRE, &on_client_pre, nullptr, &h);
    ready_producer();
    return true;
}

}  // namespace
}  // namespace kiln

KILN_SERVICE(client_crosshair, KILN_CLIENT_CROSSHAIR_SERVICE, KILN_CLIENT_CROSSHAIR_VERSION, &kiln::kTable, &kiln::init);
