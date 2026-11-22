// SPDX-License-Identifier: MIT
// Production wiring of kiln.world's named services (named_services.cpp): kiln.internal.engine's write gate and the exact
// profile's typed serviceLayout, Kiln's own server-world facts (KilnApi.session and game_world_session, sampled twice),
// the memory-range check (region_win.cpp region_writable, VirtualQuery), kiln.types for a view's type, and kiln.hooks'
// owner-retiring observer (an owner's views end with it). The rows world.named_read and world.named_write say whether
// they are on and why not; before on_ready, and on every build where they are off, every member answers unavailable.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "kiln_engine_native.h"
#include "named_services.h"
#include "services/kiln_caps.h"
#include "services/kiln_hooks.h"
#include "services/kiln_types.h"
#include "world_internal.h"
#include "wraps.h"  // game_world_session

namespace kw {
namespace {

// Kiln's own facts of the server world, sampled twice: a session clock or a world that moved between the samples is
// unreadable, and a live world with session 0 would alias "no session" on a view.
bool server_fact(void*, uint64_t* session) {
    *session = 0;
    if (!g_api || g_api->size < offsetof(KilnApi, session) + sizeof(void*) || !g_api->session) return false;
    auto read = [](KilnSessionInfo& s) -> bool {
        s = KilnSessionInfo{};
        s.size = sizeof s;
        return g_api->session(&s) == KILN_OK;
    };
    KilnSessionInfo first{}, second{};
    const uint64_t a = game_world_session(KILN_WORLD_SERVER);
    if (!read(first) || !read(second)) return false;
    const uint64_t b = game_world_session(KILN_WORLD_SERVER);
    if (a != b || first.worlds[KILN_WORLD_SERVER] != second.worlds[KILN_WORLD_SERVER] ||
        first.sessions[KILN_WORLD_SERVER] != second.sessions[KILN_WORLD_SERVER])
        return false;
    if (first.worlds[KILN_WORLD_SERVER] != 0 && a == 0) return false;
    *session = first.worlds[KILN_WORLD_SERVER] ? a : 0;
    return true;
}
uint64_t server_step(void*) {
    if (!g_api || g_api->size < offsetof(KilnApi, session) + sizeof(void*) || !g_api->session) return 0;
    KilnSessionInfo s{};
    s.size = sizeof s;
    return g_api->session(&s) == KILN_OK ? s.steps[KILN_WORLD_SERVER] : 0;
}
bool writable(void*, std::uintptr_t address, std::size_t bytes) { return region_writable(address, bytes); }
void named_log(void*, int level, const char* text) { ::kw::logf(level, "%s", text); }

// The boot instance answers unavailable until on_ready publishes the real one; both are never freed (a task thread may
// be inside one while on_ready publishes its successor).
named::Services g_boot{named::Source{}};
std::atomic<named::Services*> g_named{&g_boot};
std::atomic<bool> g_read{false}, g_write{false};

void KILN_CALL owner_retiring(KilnOwner owner, void*) { g_named.load(std::memory_order_acquire)->owner_retired(owner); }

void report_row(const char* row, uint32_t state, const std::string& text) {
    if (!g_api || !g_api->service) return;
    const auto* caps = static_cast<const KilnCapsApi*>(
        g_api->service(KilnStr{KILN_CAPS_SERVICE, sizeof(KILN_CAPS_SERVICE) - 1}, KILN_CAPS_VERSION));
    if (!caps || caps->header.size < offsetof(KilnCapsApi, report) + sizeof(void*) || !caps->report) return;
    caps->report(g_owner, KilnStr{row, std::strlen(row)}, state,
                 state == KILN_CAP_AVAILABLE ? KILN_CAP_REASON_NONE : KILN_CAP_REASON_SYMBOL_MISSING,
                 KilnStr{text.data(), text.size()});
}

}  // namespace

// kiln_plugin_main: the owner-retiring observer (a view of an owner that goes ends with it).
void named_services_init() {
    if (!g_api || !g_api->service) return;
    const auto* hooks = static_cast<const KilnHooksApi*>(
        g_api->service(KilnStr{KILN_HOOKS_SERVICE, sizeof(KILN_HOOKS_SERVICE) - 1}, KILN_HOOKS_VERSION));
    KilnHandle h = 0;
    if (!hooks || hooks->header.size < offsetof(KilnHooksApi, unsubscribe_owner_retiring) + sizeof(void*) ||
        !hooks->subscribe_owner_retiring || hooks->subscribe_owner_retiring(g_owner, &owner_retiring, nullptr, &h) != KILN_OK)
        KW_I("world.named_read: no owner-retiring observer here; a view ends only with named_service_close");
}

// on_ready: the exact profile's serviceLayout and the gate decide; the rows say which check failed.
void named_services_ready() {
    named::Source s;
    s.server = &server_fact;
    s.server_step = &server_step;
    s.writable = &writable;
    s.log = &named_log;
    char refused[160] = {};
    s.engine = kiln_engine_native(g_api, g_owner, offsetof(KilnEngineNativeApi, ecs_gate_release) + sizeof(void*), refused,
                                  sizeof refused);
    if (g_api && g_api->service) {
        const auto* types = static_cast<const KilnTypesApi*>(
            g_api->service(KilnStr{KILN_TYPES_SERVICE, sizeof(KILN_TYPES_SERVICE) - 1}, KILN_TYPES_VERSION));
        if (types && types->header.size >= offsetof(KilnTypesApi, type_info) + sizeof(void*)) s.types = types;
    }
    auto* services = new named::Services(s);  // process lifetime (see g_named)
    g_named.store(services, std::memory_order_release);
    const kiln::engine::Profile* profile = s.engine && s.engine->profile ? s.engine->profile() : nullptr;
    const std::string id = profile && profile->id ? profile->id : "";
    const char* off = s.engine ? services->unavailable_reason() : refused;
    g_read.store(off == nullptr, std::memory_order_release);
    g_write.store(off == nullptr, std::memory_order_release);
    if (!off) {
        std::string names;
        const auto& L = s.engine->layout()->services;
        for (uint32_t i = 0; i < L.contract_count && i < kiln::engine::kServiceContracts; ++i)
            names += (i ? ", " : "") + std::string(L.contracts[i].key.data(), L.contracts[i].key_size);
        report_row("world.named_read", KILN_CAP_AVAILABLE,
                   "named-service views and snapshots from any thread, between frames, under the engine's write gate "
                   "on " + id + " (" + names + ")");
        report_row("world.named_write", KILN_CAP_AVAILABLE,
                   "named-service compare-exchange writes from any thread, between frames, under the engine's write "
                   "gate on " + id + " (" + names + ")");
        KW_I("world.named_read: on (%s: %s)", id.c_str(), names.c_str());
    } else {
        const std::string why = std::string(off[0] ? off : "kiln.internal.engine is not here");
        report_row("world.named_read", KILN_CAP_UNAVAILABLE, why + "; named services report unavailable");
        report_row("world.named_write", KILN_CAP_UNAVAILABLE, why + "; named-service writes report unavailable");
        KW_I("world.named_read: off (%s)", why.c_str());
    }
}

uint32_t named_capability() {
    return (g_read.load(std::memory_order_acquire) ? KILN_WORLD_CAP_NAMED_READ : 0) |
           (g_write.load(std::memory_order_acquire) ? KILN_WORLD_CAP_NAMED_WRITE : 0);
}

KilnResult KILN_CALL t_named_service_open(KilnOwner owner, KilnStr name, uint64_t expected_session, KilnHandle* view,
                                          const KilnType** type) {
    return g_named.load(std::memory_order_acquire)->open(owner, name, expected_session, view, type);
}
KilnResult KILN_CALL t_named_service_snapshot(KilnOwner owner, KilnHandle view, void* out, uint32_t capacity,
                                              uint32_t* required) {
    return g_named.load(std::memory_order_acquire)->snapshot(owner, view, out, capacity, required);
}
KilnResult KILN_CALL t_named_service_close(KilnOwner owner, KilnHandle view) {
    return g_named.load(std::memory_order_acquire)->close(owner, view);
}
KilnResult KILN_CALL t_named_service_compare_exchange(KilnOwner owner, KilnHandle view, const void* expected,
                                                      const void* mask, const void* desired, uint32_t bytes,
                                                      void* confirmed, KilnMutationReply* reply) {
    return g_named.load(std::memory_order_acquire)
        ->compare_exchange(owner, view, expected, mask, desired, bytes, confirmed, reply);
}

}  // namespace kw
