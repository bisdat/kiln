// SPDX-License-Identifier: MIT
// kiln.ui.snapshot in kiln-core: the production environment of ui_snapshot.cpp (kiln.client's session and local player,
// the core symbol GetEntityTransform called under an SEH guard on the client world, kiln.time's get on the hosted
// server world, kiln.ui.values' pre-provider stage, kiln.caps, the owner registry, the log), its tick and world
// subscriptions, and the service's registration.
//
// Client only: on a dedicated server init() says no (no local player, no UI). Nothing reads the game at boot: the
// client world's Post and the server world's Post return at once while nobody demands anything.
#include <cstddef>
#include <cstring>

#include "../api.h"
#include "../common.h"
#include "../crash.h"
#include "../image.h"
#include "../log.h"
#include "../registry.h"
#include "../sigdb.h"
#include "../tick.h"
#include "../worldinfo.h"
#include "service.h"
#include "services/kiln_caps.h"
#include "services/kiln_client.h"
#include "services/kiln_time.h"
#include "ui_snapshot_internal.h"
#include "ui_values_internal.h"

namespace kiln::uisnapshot {
namespace {

KilnOwner g_owner = 0;
uintptr_t g_get_transform = 0;  // GetEntityTransform: const WorldTransform*(world, entity)
std::atomic<const KilnTimeApi*> g_time{nullptr};

const KilnCapsApi* caps() { return static_cast<const KilnCapsApi*>(svc::find(KILN_CAPS_SERVICE, 1)); }

bool world_now(WorldNow* out) {
    static const auto* client = static_cast<const KilnClientApi*>(svc::find(KILN_CLIENT_SERVICE, 1));
    if (!client) return false;
    KilnClientState s{};
    s.size = sizeof s;
    if (client->state(&s) != KILN_OK) return false;
    out->state = s.session;
    out->client_session = s.client_session;
    out->server_session = s.server_session;
    out->local_player = s.local_player;
    out->client_world = worldinfo::world(KILN_WORLD_CLIENT);
    return true;
}

// Holds no C++ object with a destructor, as __try requires (crash::Capture is plain data).
using TransformFn = const void* (*)(uintptr_t world, uint32_t entity);
int call_transform(uintptr_t fn, uintptr_t world, uint32_t entity, uint8_t* out) {
    crash::Capture cap{};
    unsigned long code = 0;
    __try {
        const void* p = reinterpret_cast<TransformFn>(fn)(world, entity);
        if (!p) return kReadAbsent;
        std::memcpy(out, p, kTransformBytes);
        return kReadOk;
    } __except (crash::filter(GetExceptionInformation(), &cap, &code)) {
        crash::report(cap, "kiln.ui.snapshot GetEntityTransform (client world)", reinterpret_cast<const void*>(fn));
        return kReadFault;
    }
}

int read_transform(uintptr_t world, uint32_t entity, uint8_t* out) {
    return call_transform(g_get_transform, world, entity, out);
}

// kiln.time is a service DLL: published after the core's services started, looked up at first use.
int host_hours(double* out) {
    const KilnTimeApi* t = g_time.load(std::memory_order_acquire);
    if (!t) {
        t = static_cast<const KilnTimeApi*>(api()->service(KilnStr{KILN_TIME_SERVICE, sizeof(KILN_TIME_SERVICE) - 1}, 1));
        if (!t || t->header.size < offsetof(KilnTimeApi, get) + sizeof(void*) || !t->get) return kReadFault;
        g_time.store(t, std::memory_order_release);
    }
    // a later kiln.time may fill a bigger state: give it room, read the v1 hours only when its size covers them
    struct {
        KilnTimeState s;
        uint8_t room[256];
    } buf{};
    buf.s.size = sizeof buf.s;
    if (t->get(&buf.s) != KILN_OK) return kReadAbsent;
    if (buf.s.size < offsetof(KilnTimeState, hours) + sizeof(double)) return kReadAbsent;
    *out = buf.s.hours;
    return kReadOk;
}

KilnResult stage_subscribe(uivalues::StageFn fn, void* user, KilnHandle* out) {
    return uivalues::stage_subscribe(0, fn, user, out);
}

void row_off(const char* name, uint32_t reason, const char* text) {
    const KilnCapsApi* c = caps();
    if (c && g_owner)
        c->report(g_owner, KilnStr{name, std::strlen(name)}, KILN_CAP_UNAVAILABLE, reason, KilnStr{text, std::strlen(text)});
}

void KILN_CALL on_client_post_cb(const KilnTickInfo* tick, void*) { on_client_post(tick ? tick->step : 0); }
void KILN_CALL on_server_post_cb(const KilnTickInfo* tick, void*) { on_server_post(tick ? tick->step : 0); }
void KILN_CALL on_world_cb(const KilnWorldEvent* ev, void*) {
    if (ev) on_world_event(ev->event, ev->kind, ev->session);
}

void wire() {
    Env& e = env();
    e.tick_ms = []() -> uint64_t { return GetTickCount64(); };
    e.client_side = [] { return image().side == KILN_SIDE_CLIENT; };
    e.owner_alive = [](KilnOwner o) { return reg::owner_alive(o); };
    e.now = &world_now;
    e.read_transform = g_get_transform ? &read_transform : nullptr;
    e.host_hours = &host_hours;
    e.stage_subscribe = &stage_subscribe;
    e.stage_unsubscribe = [](KilnHandle h) { return uivalues::stage_unsubscribe(h); };
    e.row_off = &row_off;
    e.log = [](int level, const char* text) { kiln::log::write(level, "kiln", text); };
}

bool init() {
    if (image().side != KILN_SIDE_CLIENT) {
        KLOG_I("ui.snapshot: client only, not started on a server");
        return false;
    }
    if (!tick::installed()) {
        KLOG_W("ui.snapshot: no tick hook on this build: no client-world Post to collect on; unavailable");
        return false;
    }
    if (!sigdb::symbol("GetEntityTransform", &g_get_transform)) g_get_transform = 0;
    wire();
    g_owner = reg::owner_create("kiln.ui.snapshot");
    if (reg::add_owner_retiring(g_owner, [](KilnOwner owner, void*) { owner_retired(owner); }, nullptr, nullptr) !=
        KILN_OK)
        return false;
    KilnHandle h = 0;
    if (tick::subscribe_world_tick(g_owner, KILN_WORLD_CLIENT, KILN_TICK_POST, &on_client_post_cb, nullptr, &h) !=
        KILN_OK) {
        KLOG_W("ui.snapshot: the client world's Post cannot be subscribed; unavailable");
        return false;
    }
    const bool host = tick::subscribe_world_tick(g_owner, KILN_WORLD_SERVER, KILN_TICK_POST, &on_server_post_cb, nullptr,
                                                 &h) == KILN_OK;
    tick::subscribe_world(g_owner, &on_world_cb, nullptr, &h);
    if (!g_get_transform)
        row_off("world.position", KILN_CAP_REASON_SYMBOL_MISSING,
                "symbol GetEntityTransform did not resolve on this game build: no local position");
    KLOG_I("ui.snapshot: GetEntityTransform %s, hosted server Post %s; collects only what is demanded",
           g_get_transform ? "resolved" : "unresolved", host ? "subscribed" : "unavailable");
    return true;
}

}  // namespace
}  // namespace kiln::uisnapshot

KILN_SERVICE(ui_snapshot, KILN_UI_SNAPSHOT_SERVICE, KILN_UI_SNAPSHOT_VERSION, kiln::uisnapshot::table(),
             &kiln::uisnapshot::init);
