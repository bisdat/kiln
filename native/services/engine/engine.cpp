// SPDX-License-Identifier: MIT
// kiln.engine: exact-build engine profile selection (public kiln.engine v1, data only) and the Kiln-internal
// primitive table kiln.internal.engine. Portable: the Windows facts of the loaded image come from facts_win.cpp.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "engine_state.h"
#include "identity.h"
#include "include/kiln_engine_native.h"
#include "profiles.g.h"
#include "services/kiln_caps.h"

namespace kiln::engine {

Stats& stats() {
    static Stats s;
    return s;
}

namespace {
struct State {
    const KilnApi* api = nullptr;
    KilnOwner owner = 0;
    ImageFacts facts;
    Selection selection;
};
State* g_state = nullptr;  // process lifetime: other services keep pointers into the selected profile

KilnStr str(const char* s) { return {s, std::strlen(s)}; }

void log(int32_t level, const std::string& text) {
    if (g_state && g_state->api && g_state->api->log) g_state->api->log(g_state->owner, level, {text.data(), text.size()});
}

void report(const KilnCapsApi* caps, const char* name, bool available, const std::string& text) {
    if (!caps || !caps->report) return;
    caps->report(g_state->owner, str(name), available ? KILN_CAP_AVAILABLE : KILN_CAP_UNAVAILABLE,
                 available ? KILN_CAP_REASON_NONE : KILN_CAP_REASON_SYMBOL_MISSING, {text.data(), text.size()});
}

KilnResult KILN_CALL identity(KilnEngineIdentity* out) {
    if (!out || out->size < sizeof(KilnEngineIdentity)) return KILN_ERR_INVALID_ARGUMENT;
    if (!g_state) return KILN_ERR_UNAVAILABLE;
    describe(g_state->facts, g_state->selection, *out);
    return KILN_OK;
}

KilnResult KILN_CALL gate_stats(KilnEngineGateStats* out) {
    if (!out || out->size < sizeof(KilnEngineGateStats)) return KILN_ERR_INVALID_ARGUMENT;
    KilnEngineGateStats s{};
    s.size = sizeof s;
    const Stats& c = stats();
    s.attempts = c.attempts.load(std::memory_order_relaxed);
    s.acquired = c.acquired.load(std::memory_order_relaxed);
    s.busy_registry = c.busy_registry.load(std::memory_order_relaxed);
    s.busy_scene = c.busy_scene.load(std::memory_order_relaxed);
    s.refused_in_frame = c.refused_in_frame.load(std::memory_order_relaxed);
    s.refused_world_thread = c.refused_world_thread.load(std::memory_order_relaxed);
    s.refused_other = c.refused_other.load(std::memory_order_relaxed);
    s.hold_ns_total = c.hold_ns_total.load(std::memory_order_relaxed);
    s.hold_ns_max = c.hold_ns_max.load(std::memory_order_relaxed);
    s.views = c.views.load(std::memory_order_relaxed);
    s.view_failures = c.view_failures.load(std::memory_order_relaxed);
    *out = s;
    return KILN_OK;
}

const Layout* KILN_CALL layout() {
    return g_state && g_state->selection.profile ? &g_state->selection.profile->layout : nullptr;
}

const KilnEngineApi g_table = {{sizeof(KilnEngineApi), KILN_ENGINE_VERSION}, &identity, &gate_stats};
const KilnEngineNativeApi g_native = {{sizeof(KilnEngineNativeApi), KILN_ENGINE_NATIVE_VERSION}, sizeof(Layout), 0,
                                      &layout};
}  // namespace

const Layout* selected_layout() { return layout(); }

void reset_for_tests() {
    delete g_state;
    g_state = nullptr;
}

}  // namespace kiln::engine

using namespace kiln::engine;

extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info) {
    try {
        if (!api || !info || info->size < sizeof(KilnPluginInfo) || api->abi_version != KILN_ABI_VERSION ||
            api->size < offsetof(KilnApi, register_service) + sizeof(api->register_service) || !api->service ||
            !api->register_service || !api->log || !api->build_info)
            return KILN_ERR_UNAVAILABLE;
        if (g_state) return KILN_ERR_INVALID_ARGUMENT;
        g_state = new State;
        g_state->api = api;
        g_state->owner = owner;
        g_state->facts = gather_facts(api);
        g_state->selection = select(g_state->facts, generated::kProfiles,
                                    sizeof generated::kProfiles / sizeof generated::kProfiles[0]);
        const Profile* p = g_state->selection.profile;
        char line[512];
        if (p) {
            std::snprintf(line, sizeof line,
                          "kiln.engine: exact engine profile %s for %s (sha256 %s, TimeDateStamp %u, SizeOfImage %u, KFC4)",
                          p->id, p->target, p->sha256, p->timestamp, p->size_of_image);
            log(KILN_LOG_INFO, line);
        } else {
            std::snprintf(line, sizeof line,
                          "kiln.engine: %s for %s (sha256 %s, TimeDateStamp %u, SizeOfImage %u, KFC%u): engine-reading members "
                          "are unavailable on this build",
                          g_state->selection.why, g_state->facts.target.c_str(),
                          g_state->facts.sha256.empty() ? "-" : g_state->facts.sha256.c_str(), g_state->facts.timestamp,
                          g_state->facts.size_of_image, g_state->facts.archive_format);
            log(KILN_LOG_WARN, line);
        }
        const auto* caps = static_cast<const KilnCapsApi*>(api->service(str(KILN_CAPS_SERVICE), 1));
        if (caps && (caps->header.size < offsetof(KilnCapsApi, report) + sizeof caps->report || !caps->report)) caps = nullptr;
        const std::string unknown = p ? std::string() : std::string(g_state->selection.why);
        const bool read = p && (p->sections & KILN_ENGINE_SECTION_OWNERS) && (p->sections & KILN_ENGINE_SECTION_ECS_READ);
        const bool write = read && (p->sections & KILN_ENGINE_SECTION_ECS_WRITE);
        report(caps, "engine.profile", p != nullptr, p ? p->id : unknown);
        report(caps, "engine.ecs_read", read, read ? "" : p ? "the profile has no validated ECS read layout" : unknown);
        report(caps, "engine.ecs_write", write,
               write ? "" : p ? "the profile has no enabled native-thread Scene write gate" : unknown);
        info->id = str(KILN_ENGINE_SERVICE);
        info->version = str("1");
        const KilnResult rc = api->register_service(owner, str(KILN_ENGINE_SERVICE), &g_table);
        if (rc != KILN_OK) return rc;
        if (api->register_service(owner, str(KILN_ENGINE_NATIVE_SERVICE), &g_native) != KILN_OK)
            log(KILN_LOG_ERROR,
                "kiln.engine: the core refused kiln.internal.engine (kiln-engine.dll must be loaded from kiln/services); "
                "Kiln services that read engine memory report themselves unavailable");
        return KILN_OK;
    } catch (...) {
        return KILN_ERR_INTERNAL;
    }
}
