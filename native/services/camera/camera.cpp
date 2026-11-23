// SPDX-License-Identifier: MIT
// kiln.camera (services/kiln_camera.h): camera overrides as leases. docs/kiln/services/camera.md.
//
// This file is the service around the lease model (state.h): the table, the world lifecycle and ticks that drive the
// model, the owner-retiring observer, and the capability rows. The physical sides plug in as backends (state.h,
// Backend): the server's mode maintainer, the client's render transaction, the transport bridge. An endpoint without
// a backend answers "unavailable" and its rows say why.
//
// Rows (docs/kiln/services/caps.md): camera.service (this table), and one per capability bit: camera.server_mode,
// camera.local_pose, camera.local_fov, camera.local_profile, camera.local_shake, camera.rendered_snapshot,
// camera.remote, camera.resource_profile (reserved). Every write row needs kiln.hooks' owner-retiring observer
// (hooks.owner_cleanup): without it a lease could call into a mod that is gone, so nothing is admitted.
#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "kiln_api.h"
#include "services/kiln_camera.h"
#include "services/kiln_caps.h"
#include "services/kiln_hooks.h"

#include "abi_asserts.h"
#include "seh.h"
#include "state.h"

namespace {

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
const KilnCapsApi* g_caps = nullptr;
bool g_client_side = false;

#define KC_STR(s) KilnStr{s, sizeof(s) - 1}

void logf(int32_t level, const char* fmt, ...) {
    char text[768];
    va_list v;
    va_start(v, fmt);
    const int n = std::vsnprintf(text, sizeof text, fmt, v);
    va_end(v);
    if (n > 0) g_api->log(g_owner, level, KilnStr{text, static_cast<size_t>(n < static_cast<int>(sizeof text) ? n : static_cast<int>(sizeof text) - 1)});
}

struct ServiceEnv : kcam::Env {
    bool call_done(KilnCameraDone fn, const KilnCameraResult& result, void* user) override { return kcam::seh_done(fn, &result, user); }
    bool call_changed(KilnCameraChanged fn, const KilnCameraChange& change, void* user) override { return kcam::seh_changed(fn, &change, user); }
    void log(int32_t level, const std::string& text) override { g_api->log(g_owner, level, KilnStr{text.data(), text.size()}); }
};

ServiceEnv g_env;
kcam::Core g_core(&g_env);

// Which worlds this thread steps. A dedicated server has no client world.
uint32_t threads() {
    uint32_t t = 0;
    if (g_api->on_world_thread(KILN_WORLD_SERVER)) t |= kcam::kOnServer;
    if (g_client_side && g_api->on_world_thread(KILN_WORLD_CLIENT)) t |= kcam::kOnClient;
    return t;
}

// ---- the table ---------------------------------------------------------------------------------------------------------

KilnResult KILN_CALL t_capabilities(uint32_t endpoint, KilnEntity player, uint64_t* mask) { return g_core.capabilities(endpoint, player, mask); }

KilnResult KILN_CALL t_apply(KilnOwner owner, const KilnCameraRequest* request, KilnCameraLease* out_lease, KilnCameraDone done, void* user) {
    return g_core.apply(owner, request, out_lease, done, user, threads());
}

KilnResult KILN_CALL t_update(KilnOwner owner, KilnCameraLease lease, uint32_t timing, const KilnCameraRequest* request, KilnCameraDone done, void* user) {
    return g_core.update(owner, lease, timing, request, done, user, threads());
}

KilnResult KILN_CALL t_end(KilnOwner owner, KilnCameraLease lease, KilnCameraDone done, void* user) { return g_core.end(owner, lease, done, user, threads()); }

KilnResult KILN_CALL t_subscribe(KilnOwner owner, KilnCameraChanged fn, void* user, KilnHandle* out) { return g_core.subscribe(owner, fn, user, out); }

KilnResult KILN_CALL t_unsubscribe(KilnHandle handle) { return g_core.unsubscribe(handle); }

// The frame snapshot and the transport bridge are not in this build yet: their rows are off and say so.
KilnResult KILN_CALL t_get_visual(KilnCameraVisual* out, uint32_t* out_reason) {
    if (!out || !out_reason || out->size < sizeof(KilnCameraVisual)) return KILN_ERR_INVALID_ARGUMENT;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL t_bind_transport(KilnOwner, const KilnCameraTransport*) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL t_unbind_transport(KilnOwner) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL t_transport_ended(KilnOwner, KilnCameraLease, uint64_t, uint32_t, uint32_t, KilnStr) { return KILN_ERR_UNAVAILABLE; }

const KilnCameraApi kTable = {{sizeof(KilnCameraApi), KILN_CAMERA_VERSION},
                              &t_capabilities,
                              &t_apply,
                              &t_update,
                              &t_end,
                              &t_subscribe,
                              &t_unsubscribe,
                              &t_get_visual,
                              &t_bind_transport,
                              &t_unbind_transport,
                              &t_transport_ended};

// ---- what drives the model ---------------------------------------------------------------------------------------------

// STARTED: on the world's thread, before its first Pre. STOPPING: on the thread that destroys the world.
void KILN_CALL on_world(const KilnWorldEvent* e, void*) {
    if (!e) return;
    kcam::World w;
    if (e->kind == KILN_WORLD_SERVER) w = kcam::kServer;
    else if (e->kind == KILN_WORLD_CLIENT) w = kcam::kClient;
    else return;  // the prediction world has no camera of its own
    if (e->event == KILN_WORLD_STARTED) g_core.world_started(w, e->session);
    else if (e->event == KILN_WORLD_STOPPING) g_core.world_stopping(w, e->session);
}

void KILN_CALL on_server_pre(const KilnTickInfo*, void*) { g_core.pre(kcam::kServer); }
void KILN_CALL on_client_pre(const KilnTickInfo*, void*) { g_core.pre(kcam::kClient); }

// Any thread (the one that revoked the owner). Marks only: each world drops the owner's leases at its next Pre.
void KILN_CALL on_owner_retiring(KilnOwner retiring, void*) { g_core.owner_retiring(retiring); }

// ---- rows --------------------------------------------------------------------------------------------------------------

void row(const char* name, bool on, uint32_t reason, const char* text) {
    if (!g_caps || !g_caps->report) return;
    const KilnStr n{name, std::strlen(name)};
    if (on) g_caps->report(g_owner, n, KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, KilnStr{"", 0});
    else g_caps->report(g_owner, n, KILN_CAP_UNAVAILABLE, reason, KilnStr{text, std::strlen(text)});
}

void report_rows() {
    static constexpr char kNoObserver[] = "kiln.hooks has no owner cleanup (hooks.owner_cleanup): camera writes are off";
    static constexpr char kNoBackend[] = "this build of kiln.camera does not have it yet";
    static constexpr char kClientOnly[] = "the local camera of a game client";
    const bool writes = g_core.writes();
    uint64_t mode = 0, local = 0, remote = 0;
    g_core.capabilities(KILN_CAMERA_SERVER_MODE, 0, &mode);
    g_core.capabilities(KILN_CAMERA_LOCAL_VISUAL, 0, &local);
    g_core.capabilities(KILN_CAMERA_REMOTE_VISUAL, 0, &remote);
    row("camera.service", true, 0, "");
    const auto write_row = [&](const char* name, uint64_t have, uint64_t bit, bool client_only) {
        if (have & bit) row(name, true, 0, "");
        else if (client_only && !g_client_side) row(name, false, KILN_CAP_REASON_WRONG_SIDE, kClientOnly);
        else if (!writes) row(name, false, KILN_CAP_REASON_SERVICE_OLDER, kNoObserver);
        else row(name, false, KILN_CAP_REASON_SERVICE_MISSING, kNoBackend);
    };
    write_row("camera.server_mode", mode, KILN_CAMERA_CAP_SERVER_MODE, false);
    write_row("camera.local_pose", local, KILN_CAMERA_CAP_LOCAL_POSE, true);
    write_row("camera.local_fov", local, KILN_CAMERA_CAP_LOCAL_FOV, true);
    write_row("camera.local_profile", local, KILN_CAMERA_CAP_LOCAL_PROFILE, true);
    write_row("camera.local_shake", local, KILN_CAMERA_CAP_LOCAL_SHAKE, true);
    write_row("camera.remote", remote, KILN_CAMERA_CAP_REMOTE, false);
    if (local & KILN_CAMERA_CAP_RENDERED_SNAPSHOT) row("camera.rendered_snapshot", true, 0, "");
    else if (!g_client_side) row("camera.rendered_snapshot", false, KILN_CAP_REASON_WRONG_SIDE, kClientOnly);
    else row("camera.rendered_snapshot", false, KILN_CAP_REASON_SERVICE_MISSING, kNoBackend);
    row("camera.resource_profile", false, KILN_CAP_REASON_POLICY, "reserved: stock camera resource profiles are not part of kiln.camera v1");
}

// ---- start -------------------------------------------------------------------------------------------------------------

void KILN_CALL on_ready(void*) {
    g_caps = static_cast<const KilnCapsApi*>(g_api->service(KC_STR(KILN_CAPS_SERVICE), KILN_CAPS_VERSION));
    // The observer's two slots are at the table's end: ask for the size that covers them, not for the whole table of
    // this build (a later core may be larger, an older one ends before them).
    const auto* hooks = static_cast<const KilnHooksApi*>(g_api->service(KC_STR(KILN_HOOKS_SERVICE), 2));
    constexpr size_t kObserverEnd = offsetof(KilnHooksApi, unsubscribe_owner_retiring) + sizeof(void*);
    KilnHandle h = 0;
    const bool observer = hooks && hooks->header.size >= kObserverEnd && hooks->subscribe_owner_retiring && g_api->feature(KC_STR("hooks.owner_cleanup")) &&
                          hooks->subscribe_owner_retiring(g_owner, &on_owner_retiring, nullptr, &h) == KILN_OK;
    g_core.set_writes(observer);
    if (!observer) logf(KILN_LOG_WARN, "camera: kiln.hooks has no owner-retiring observer (hooks.owner_cleanup): no camera write is admitted");
    report_rows();
    uint64_t mode = 0, local = 0;
    g_core.capabilities(KILN_CAMERA_SERVER_MODE, 0, &mode);
    g_core.capabilities(KILN_CAMERA_LOCAL_VISUAL, 0, &local);
    logf(KILN_LOG_INFO, "camera: ready (%s; owner cleanup %s; capability bits: server 0x%llx, local 0x%llx)", g_client_side ? "client" : "dedicated server",
         observer ? "on" : "missing", static_cast<unsigned long long>(mode), static_cast<unsigned long long>(local));
}

}  // namespace

extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info) {
    if (api->abi_version != KILN_ABI_VERSION || api->size < sizeof(KilnApi)) return KILN_ERR_UNAVAILABLE;
    g_api = api;
    g_owner = owner;
    g_client_side = api->side() == KILN_SIDE_CLIENT;
    info->id = KC_STR(KILN_CAMERA_SERVICE);
    info->version = KilnStr{"1", 1};
    info->on_ready = &on_ready;
    // A lease belongs to one world and ends with it: without the world's start and stop there is no service.
    KilnHandle h = 0;
    if (!api->feature(KC_STR("world.lifecycle")) || api->subscribe_world(owner, &on_world, nullptr, &h) != KILN_OK) {
        logf(KILN_LOG_WARN, "camera: world lifecycle is unavailable on this build: kiln.camera is unavailable");
        return KILN_ERR_UNAVAILABLE;
    }
    if (api->subscribe_world_tick(owner, KILN_WORLD_SERVER, KILN_TICK_PRE, &on_server_pre, nullptr, &h) != KILN_OK ||
        (g_client_side && api->subscribe_world_tick(owner, KILN_WORLD_CLIENT, KILN_TICK_PRE, &on_client_pre, nullptr, &h) != KILN_OK)) {
        logf(KILN_LOG_WARN, "camera: the world ticks cannot be subscribed: kiln.camera is unavailable");
        return KILN_ERR_UNAVAILABLE;
    }
    return api->register_service(owner, KC_STR(KILN_CAMERA_SERVICE), &kTable);
}
