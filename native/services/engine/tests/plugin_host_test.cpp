// SPDX-License-Identifier: MIT
// The real kiln-engine entry over a fake core: refusals of short/old tables, the public and internal tables, the
// capability rows on an exact and an unknown build, and the consumer helper's version/size checks.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "engine_state.h"
#include "include/kiln_engine_native.h"
#include "profiles.g.h"
#include "services/kiln_caps.h"

extern "C" KilnResult KILN_CALL kiln_plugin_main(const KilnApi*, KilnOwner, KilnPluginInfo*);

namespace {
using namespace kiln::engine;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}
ImageFacts g_facts;
struct Row { uint32_t state, reason; std::string text; };
std::map<std::string, Row> g_rows;
std::map<std::string, const void*> g_tables;
std::string g_last_log;
bool g_refuse_internal = false;
KilnCapsApi g_caps{};

KilnResult KILN_CALL cap_report(KilnOwner, KilnStr n, uint32_t s, uint32_t r, KilnStr t) {
    g_rows[std::string(n.ptr, n.len)] = {s, r, std::string(t.ptr ? t.ptr : "", t.len)};
    return KILN_OK;
}
const void* KILN_CALL lookup(KilnStr n, uint32_t v) {
    const std::string name(n.ptr, n.len);
    if (name == KILN_CAPS_SERVICE) return &g_caps;
    const auto at = g_tables.find(name);
    if (at == g_tables.end()) return nullptr;
    return static_cast<const KilnServiceHeader*>(at->second)->version >= v ? at->second : nullptr;
}
KilnResult KILN_CALL publish(KilnOwner, KilnStr n, const void* t) {
    const std::string name(n.ptr, n.len);
    if (g_refuse_internal && name.rfind("kiln.internal.", 0) == 0) return KILN_ERR_INVALID_ARGUMENT;
    if (g_tables.count(name)) return KILN_ERR_INVALID_ARGUMENT;
    g_tables[name] = t;
    return KILN_OK;
}
void KILN_CALL logger(KilnOwner, int32_t, KilnStr text) { g_last_log.assign(text.ptr, text.len); }
KilnResult KILN_CALL build(KilnBuildInfo* out) { out->side = KILN_SIDE_CLIENT; return KILN_OK; }

KilnApi make_api() {
    KilnApi api{};
    api.size = sizeof api; api.abi_version = KILN_ABI_VERSION;
    api.service = &lookup; api.register_service = &publish; api.log = &logger; api.build_info = &build;
    return api;
}
ImageFacts facts_of(const Profile& p) {
    ImageFacts f;
    f.headers = true; f.target = p.target; f.sha256 = p.sha256; f.timestamp = p.timestamp; f.size_of_image = p.size_of_image;
    f.archive_format = 4; f.side = std::string(p.target) == "enshrouded.exe" ? KILN_SIDE_CLIENT : KILN_SIDE_SERVER;
    return f;
}
void fresh() {
    reset_for_tests();
    g_rows.clear(); g_tables.clear(); g_last_log.clear(); g_refuse_internal = false;
}
KilnResult boot(const KilnApi& api) {
    KilnPluginInfo info{}; info.size = sizeof info;
    return kiln_plugin_main(&api, 7, &info);
}

void refusals() {
    fresh();
    KilnApi api = make_api();
    KilnPluginInfo info{}; info.size = 8;
    check(kiln_plugin_main(&api, 7, &info) == KILN_ERR_UNAVAILABLE, "short plugin info refused");
    check(kiln_plugin_main(nullptr, 7, &info) == KILN_ERR_UNAVAILABLE, "null API refused");
    KilnApi old = api; old.size = offsetof(KilnApi, register_service);
    info.size = sizeof info;
    check(kiln_plugin_main(&old, 7, &info) == KILN_ERR_UNAVAILABLE, "API prefix without register_service refused");
    KilnApi other = api; other.abi_version = KILN_ABI_VERSION + 1;
    check(kiln_plugin_main(&other, 7, &info) == KILN_ERR_UNAVAILABLE, "other ABI version refused");
    check(g_tables.empty(), "nothing published by a refused boot");
}

void exact_build(const Profile& p) {
    fresh();
    g_facts = facts_of(p);
    KilnApi api = make_api();
    check(boot(api) == KILN_OK, std::string(p.id) + ": boot");
    const auto* t = static_cast<const KilnEngineApi*>(lookup({KILN_ENGINE_SERVICE, 11}, 1));
    check(t && t->header.size == sizeof(KilnEngineApi) && t->identity && t->gate_stats, "public table published");
    KilnEngineIdentity id{}; id.size = 8;
    check(t && t->identity(&id) == KILN_ERR_INVALID_ARGUMENT, "short identity record refused");
    check(t && t->identity(nullptr) == KILN_ERR_INVALID_ARGUMENT, "null identity refused");
    id.size = sizeof id;
    check(t && t->identity(&id) == KILN_OK && id.status == KILN_ENGINE_EXACT && std::string(id.profile_id) == p.id &&
              id.sections == p.sections, std::string(p.id) + ": identity exact");
    KilnEngineGateStats st{}; st.size = sizeof st;
    check(t && t->gate_stats(&st) == KILN_OK && st.size == sizeof st && !st.attempts, "stats copy");
    check(g_rows["engine.profile"].state == KILN_CAP_AVAILABLE && g_rows["engine.profile"].text == p.id, "engine.profile row");
    check(g_rows["engine.ecs_read"].state == KILN_CAP_AVAILABLE && g_rows["engine.ecs_write"].state == KILN_CAP_AVAILABLE,
          "ecs rows available on the exact build");
    char why[160]{};
    const KilnEngineNativeApi* native = kiln_engine_native(&api, 7, sizeof(KilnEngineNativeApi), why, sizeof why);
    check(native && native->layout && native->layout() == &p.layout, "internal table hands out the selected layout");
    // version and layout-size mismatches refuse loudly (a consumer from another Kiln build)
    KilnEngineNativeApi copy = *native;
    g_tables[KILN_ENGINE_NATIVE_SERVICE] = &copy;
    copy.layout_size = sizeof(Layout) + 8;
    g_last_log.clear();
    check(!kiln_engine_native(&api, 7, sizeof copy, why, sizeof why) && std::strstr(why, "another Kiln build"), "layout size mismatch refused");
    check(g_last_log.find("kiln.internal.engine refused: ") == 0, "a refusal is logged through api->log");
    copy = *native; copy.header.version = KILN_ENGINE_NATIVE_VERSION + 1;
    check(!kiln_engine_native(&api, 7, sizeof copy, why, sizeof why), "newer internal version refused");
    copy = *native; copy.header.size = offsetof(KilnEngineNativeApi, layout);
    check(!kiln_engine_native(&api, 7, sizeof(KilnEngineNativeApi), why, sizeof why), "truncated internal table refused");
    copy = *native;
    check(!kiln_engine_native(&api, 7, sizeof copy + 8, why, sizeof why), "a member past the table refused");
    KilnPluginInfo again{}; again.size = sizeof again;
    check(kiln_plugin_main(&api, 8, &again) == KILN_ERR_INVALID_ARGUMENT, "second boot refused");
}

void unknown_build() {
    fresh();
    g_facts = facts_of(generated::kProfiles[0]);
    g_facts.timestamp += 1;
    KilnApi api = make_api();
    check(boot(api) == KILN_OK, "unknown build still boots");
    const auto* t = static_cast<const KilnEngineApi*>(lookup({KILN_ENGINE_SERVICE, 11}, 1));
    KilnEngineIdentity id{}; id.size = sizeof id;
    check(t && t->identity(&id) == KILN_OK && id.status == KILN_ENGINE_UNKNOWN_BUILD && !id.profile_id[0] && !id.sections,
          "identity unknown build");
    for (const char* row : {"engine.profile", "engine.ecs_read", "engine.ecs_write"})
        check(g_rows[row].state == KILN_CAP_UNAVAILABLE && g_rows[row].reason == KILN_CAP_REASON_SYMBOL_MISSING &&
                  !g_rows[row].text.empty(), std::string(row) + " unavailable with a reason");
    check(g_last_log.find("unavailable on this build") != std::string::npos, "unknown build logged");
    char why[160]{};
    const auto* native = kiln_engine_native(&api, 7, sizeof(KilnEngineNativeApi), why, sizeof why);
    check(native && !native->layout(), "no layout without an exact profile");
}

void internal_refused() {
    fresh();
    g_facts = facts_of(generated::kProfiles[0]);
    g_refuse_internal = true;  // the core refuses: kiln-engine.dll was not loaded from kiln/services
    KilnApi api = make_api();
    check(boot(api) == KILN_OK, "public service survives a refused internal table");
    check(g_last_log.find("refused kiln.internal.engine") != std::string::npos, "refusal logged loudly");
    char why[160]{};
    check(!kiln_engine_native(&api, 7, sizeof(KilnEngineNativeApi), why, sizeof why) && std::strstr(why, "not here"),
          "consumers see no internal table and say why");
}
}  // namespace

namespace kiln::engine {
ImageFacts gather_facts(const KilnApi*) { return g_facts; }
}  // namespace kiln::engine

int main() {
    g_caps.header = {sizeof g_caps, KILN_CAPS_VERSION};
    g_caps.report = &cap_report;
    refusals();
    for (const auto& p : generated::kProfiles) exact_build(p);
    unknown_build();
    internal_refused();
    reset_for_tests();
    std::printf("engine plugin host: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
