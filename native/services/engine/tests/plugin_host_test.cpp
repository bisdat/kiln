// SPDX-License-Identifier: MIT
// The real kiln-engine entry over a fake core: refusals of short/old tables, the public and internal tables, the
// capability rows on an exact and an unknown build, the consumer helper's version/size checks, and the internal read
// members (copy, ecs_view, ecs_write_supported, profile) wired to the real resolution over a fake engine, and the
// gate members: caller-thread refusals from KilnApi.on_world_thread before any word, the RAII scope, and no KilnApi call
// or log between acquire and release (every fake KilnApi function counts its calls); the content worker's lifetime guard
// members: argument checks, an owned guard's caller-thread refusals, a borrowed guard inside a gate's hold with no
// KilnApi call, and a consumer of another build's layout refused; the engine call: the member at 136, its argument
// checks, a call through the scope with no KilnApi call or log inside the hold, a fault latched and logged once after
// the release, and the server world's STARTED clearing the latch.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "engine_state.h"
#include "fake_engine.h"
#include "gate.h"
#include "include/kiln_engine_native.h"
#include "invoke.h"
#include "profiles.g.h"
#include "services/kiln_caps.h"
#include "../../../core/src/launch_internal.h"

extern "C" KilnResult KILN_CALL kiln_plugin_main(const KilnApi*, KilnOwner, KilnPluginInfo*);
namespace kiln::engine::mem {
extern std::atomic<std::uintptr_t> test_image;
extern std::atomic<std::uint64_t> test_reads, test_cas;
extern std::atomic<int> test_tracker_calls;
extern thread_local int test_priority;
extern thread_local int test_boosts;
}
namespace kiln::engine::call {
extern std::atomic<Return (*)(std::uintptr_t, const std::uint64_t*)> test_call_hook;
extern std::atomic<std::uint64_t> test_calls;
}

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
std::map<std::string, int> g_lookups;  // lookups per name (fresh() clears them)
std::string g_last_log;
bool g_refuse_internal = false;
int g_api_calls = 0;      // every fake KilnApi function counts here
int g_world_thread = -1;  // the KILN_WORLD_* kind the caller "is on", -1 = none
std::uint64_t g_step = 100;  // the server step the fake caller observed; each scope below uses a new one
KilnWorldEventFn g_world_fn = nullptr;  // the engine's world lifecycle subscription
void* g_world_user = nullptr;
KilnCapsApi g_caps{};

KilnResult KILN_CALL cap_report(KilnOwner, KilnStr n, uint32_t s, uint32_t r, KilnStr t) {
    g_rows[std::string(n.ptr, n.len)] = {s, r, std::string(t.ptr ? t.ptr : "", t.len)};
    return KILN_OK;
}
const void* KILN_CALL lookup(KilnStr n, uint32_t v) {
    ++g_api_calls;
    const std::string name(n.ptr, n.len);
    ++g_lookups[name];
    if (name == KILN_CAPS_SERVICE) return &g_caps;
    const auto at = g_tables.find(name);
    if (at == g_tables.end()) return nullptr;
    return static_cast<const KilnServiceHeader*>(at->second)->version >= v ? at->second : nullptr;
}
KilnResult KILN_CALL publish(KilnOwner, KilnStr n, const void* t) {
    ++g_api_calls;
    const std::string name(n.ptr, n.len);
    if (g_refuse_internal && name.rfind("kiln.internal.", 0) == 0) return KILN_ERR_INVALID_ARGUMENT;
    if (g_tables.count(name)) return KILN_ERR_INVALID_ARGUMENT;
    g_tables[name] = t;
    return KILN_OK;
}
std::vector<std::pair<int32_t, std::string>> g_logs;  // every line of the current boot (fresh() clears it)
void KILN_CALL logger(KilnOwner, int32_t level, KilnStr text) {
    ++g_api_calls;
    g_last_log.assign(text.ptr, text.len);
    g_logs.emplace_back(level, g_last_log);
}
// KILN_PATH_DATA: a scratch folder of this test (the measurement switch's file lives below it); other kinds unknown.
std::string g_data_dir;
KilnResult KILN_CALL path_of(int32_t kind, char* buffer, size_t capacity, size_t* out_len) {
    ++g_api_calls;
    if (kind != KILN_PATH_DATA || g_data_dir.empty()) return KILN_ERR_NOT_FOUND;
    *out_len = g_data_dir.size();
    if (capacity < g_data_dir.size()) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(buffer, g_data_dir.data(), g_data_dir.size());
    return KILN_OK;
}
KilnResult KILN_CALL build(KilnBuildInfo* out) { ++g_api_calls; out->side = KILN_SIDE_CLIENT; return KILN_OK; }
bool KILN_CALL on_world(uint32_t kind) { ++g_api_calls; return g_world_thread >= 0 && kind == static_cast<uint32_t>(g_world_thread); }
KilnResult KILN_CALL subscribe_world(KilnOwner, KilnWorldEventFn fn, void* user, KilnHandle* out) {
    ++g_api_calls;
    g_world_fn = fn;
    g_world_user = user;
    *out = KilnHandle{};
    return KILN_OK;
}
void world(uint32_t event) {
    KilnWorldEvent ev{};
    ev.size = sizeof ev;
    ev.event = event;
    ev.kind = KILN_WORLD_SERVER;
    ev.session = 1;
    if (g_world_fn) g_world_fn(&ev, g_world_user);
}
KilnPluginInfo g_info{};  // the last boot's plugin info (on_unload)
KilnTickFn g_tick_fn = nullptr;  // the engine's server Pre subscription (the live step for the busy latch)
void* g_tick_user = nullptr;
int32_t g_tick_phase = -1;
KilnResult KILN_CALL subscribe_tick(KilnOwner, int32_t phase, KilnTickFn fn, void* user, KilnHandle* out) {
    ++g_api_calls;
    g_tick_fn = fn;
    g_tick_user = user;
    g_tick_phase = phase;
    *out = KilnHandle{};
    return KILN_OK;
}
void server_pre(uint64_t step) {
    KilnTickInfo info{};
    info.size = sizeof info;
    info.world_kind = KILN_WORLD_SERVER;
    info.step = step;
    if (g_tick_fn) g_tick_fn(&info, g_tick_user);
}

KilnApi make_api() {
    KilnApi api{};
    api.size = sizeof api; api.abi_version = KILN_ABI_VERSION;
    api.service = &lookup; api.register_service = &publish; api.log = &logger; api.build_info = &build;
    api.on_world_thread = &on_world;
    api.path = &path_of;
    api.subscribe_world = &subscribe_world;
    api.subscribe_tick = &subscribe_tick;
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
    g_rows.clear(); g_tables.clear(); g_lookups.clear(); g_last_log.clear(); g_logs.clear(); g_refuse_internal = false; g_world_thread = -1;
}
KilnResult boot(const KilnApi& api) {
    g_info = KilnPluginInfo{};
    g_info.size = sizeof g_info;
    return kiln_plugin_main(&api, 7, &g_info);
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

std::uint64_t views() {
    KilnEngineGateStats st{}; st.size = sizeof st;
    static_cast<const KilnEngineApi*>(lookup({KILN_ENGINE_SERVICE, 11}, 1))->gate_stats(&st);
    return st.views * 1000 + st.view_failures;
}

// The read members on an exact build: the table reaches the real copy and resolution (resolve_test covers every
// refusal; here: the wiring, argument checks, reasons and counters).
void gate_members(const KilnEngineNativeApi& n, KilnApi& api, const Profile& p, fake::Engine& e, const EcsView& view);
void guard_members(const KilnEngineNativeApi& n, KilnApi& api, const Profile& p, fake::Engine& e, const EcsView& view);
void invoke_members(const KilnEngineNativeApi& n, const Profile& p, fake::Engine& e, const EcsView& view);
void primitives(const KilnEngineNativeApi& n, KilnApi& api, const Profile& p) {
    const std::string id = std::string(p.id) + ": ";
    check(n.header.size == sizeof(KilnEngineNativeApi) && n.copy && n.ecs_view && n.ecs_write_supported && n.profile,
          id + "every internal member present");
    check(n.profile() == &p && std::string(n.profile()->attribute_calculation_model) == "attribute-command-v1",
          id + "profile() is the selected profile with its published attribute model");
    fake::Region r(4096);
    fake::put<std::uint64_t>(r.at(16), 0x0123456789abcdefull);
    std::uint64_t v = 0;
    check(n.copy(r.at(16), &v, sizeof v) == KILN_OK && v == 0x0123456789abcdefull, id + "copy reads");
    check(n.copy(fake::unmapped(), &v, sizeof v) == KILN_ERR_UNAVAILABLE, id + "copy of an unreadable page fails");
    check(n.copy(0, &v, sizeof v) == KILN_ERR_UNAVAILABLE && n.copy(UINTPTR_MAX - 2, &v, sizeof v) == KILN_ERR_UNAVAILABLE,
          id + "copy of a null or overflowing range fails");
    check(n.copy(r.at(), nullptr, 8) == KILN_ERR_INVALID_ARGUMENT && n.copy(0, nullptr, 0) == KILN_OK,
          id + "copy: no out buffer refused, zero bytes is a no-op");
    fake::Engine e(p.layout);
    kiln::engine::mem::test_image = e.image.at();
    EcsView view{};
    std::uint32_t reason = 99;
    const std::uint64_t before = views();
    check(n.ecs_view(0, &view, &reason) == KILN_OK && reason == 0 && view.role == Role::LocalAuthority &&
              view.manager == e.manager() && view.owners.image == e.image.at(),
          id + "authority view through the table");
    check(views() == before + 1000, id + "a view is counted");
    check(n.ecs_write_supported(&view, 3) == 1 && n.ecs_write_supported(&view, 128) == 0 &&
              n.ecs_write_supported(nullptr, 3) == 0,
          id + "write support through the table");
    EcsView client{};
    reason = 99;
    if (p.layout.dedicated_server) {
        check(n.ecs_view(1, &client, &reason) == KILN_ERR_UNAVAILABLE &&
                  reason == static_cast<std::uint32_t>(Reason::DedicatedServerHasNoLocalClientInput) &&
                  views() == before + 2001,
              id + "no client view on a dedicated server: reason and failure counted");
    } else {
        check(n.ecs_view(1, &client, &reason) == KILN_OK && client.role == Role::ClientReplica &&
                  n.ecs_write_supported(&client, 3) == 0,
              id + "client view through the table, never writable");
    }
    check(n.ecs_view(2, &client, &reason) == KILN_ERR_INVALID_ARGUMENT && n.ecs_view(0, nullptr, &reason) == KILN_ERR_INVALID_ARGUMENT &&
              n.ecs_view(0, &client, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          id + "unknown kind and null records refused");
    *fake::word(e.registry_word()) = static_cast<std::int32_t>(0x80000000u);
    if (p.layout.dedicated_server)
        check(n.ecs_view(0, &client, &reason) == KILN_ERR_UNAVAILABLE &&
                  reason == static_cast<std::uint32_t>(Reason::EcsThreadRegistryBusy),
              id + "a busy registry word refuses at once with ecs-thread-registry-busy");
    *fake::word(e.registry_word()) = 0;
    guard_members(n, api, p, e, view);  // before gate_members, whose exit check closes the gate for good
    invoke_members(n, p, e, view);
    gate_members(n, api, p, e, view);
    kiln::engine::mem::test_image = 0;
}

// The content worker's lifetime guard through the real table over a fake engine (resource_guard_test covers every
// refusal of the guard itself).
void guard_members(const KilnEngineNativeApi& n, KilnApi& api, const Profile& p, fake::Engine& e, const EcsView& view) {
    const std::string id = std::string(p.id) + ": guard: ";
    e.add_content_worker();
    const auto words_free = [&] { return *fake::word(e.registry_word()) == 0 && *fake::word(e.scene_word()) == 0; };
    check(n.header.size == sizeof(KilnEngineNativeApi) && n.resource_guard_acquire && n.resource_guard_validate &&
              n.resource_guard_contains && n.resource_guard_release,
          id + "the four members present");
    // argument checks: refused before anything, the record untouched
    {
        ResourceGuard g{};
        std::uint32_t reason = 77;
        check(n.resource_guard_acquire(nullptr, e.service(), e.reader(), &g, &reason) == KILN_ERR_INVALID_ARGUMENT &&
                  reason == 77,
              id + "a record without its size refused");
        g.size = sizeof g;
        check(n.resource_guard_acquire(nullptr, 0, e.reader(), &g, &reason) == KILN_ERR_INVALID_ARGUMENT &&
                  n.resource_guard_acquire(nullptr, e.service(), 0, &g, &reason) == KILN_ERR_INVALID_ARGUMENT &&
                  n.resource_guard_acquire(nullptr, e.service(), e.reader(), nullptr, &reason) == KILN_ERR_INVALID_ARGUMENT &&
                  n.resource_guard_acquire(nullptr, e.service(), e.reader(), &g, nullptr) == KILN_ERR_INVALID_ARGUMENT,
              id + "a service or reader of 0 and NULL records refused");
        EcsGate short_gate{};
        short_gate.size = 8;
        check(n.resource_guard_acquire(&short_gate, e.service(), e.reader(), &g, &reason) == KILN_ERR_INVALID_ARGUMENT &&
                  !g.held && words_free(),
              id + "a gate record of the wrong size refused");
        check(!n.resource_guard_validate(nullptr) && !n.resource_guard_contains(nullptr, e.payload.at(), 1) &&
                  !n.resource_guard_validate(&g) && !n.resource_guard_contains(&g, e.payload.at(), 1),
              id + "validate and contains of NULL or an unheld record answer 0");
        n.resource_guard_release(nullptr);
        ResourceGuard odd{};
        odd.size = 8;
        odd.registry_lock = e.registry_word();
        odd.owns_registry = 1;
        n.resource_guard_release(&odd);
        check(words_free() && odd.registry_lock == e.registry_word(), id + "release of NULL or a wrong-size record: a no-op");
    }
    // owned: the caller's thread from the core, before any engine read or CAS
    for (const auto& [kind, want] : {std::pair{static_cast<int>(KILN_WORLD_SERVER), Reason::InFrame},
                                     std::pair{static_cast<int>(KILN_WORLD_CLIENT), Reason::WorldThread},
                                     std::pair{static_cast<int>(KILN_WORLD_PREDICTION), Reason::WorldThread}}) {
        g_world_thread = kind;
        ResourceGuard g{};
        g.size = sizeof g;
        std::uint32_t reason = 0;
        const std::uint64_t reads = kiln::engine::mem::test_reads.load(), cas = kiln::engine::mem::test_cas.load();
        check(n.resource_guard_acquire(nullptr, e.service(), e.reader(), &g, &reason) == KILN_ERR_UNAVAILABLE &&
                  reason == static_cast<std::uint32_t>(want) && !g.held && kiln::engine::mem::test_reads.load() == reads &&
                  kiln::engine::mem::test_cas.load() == cas && words_free(),
              id + "owned on world kind " + std::to_string(kind) + ": " + reason_name(want) + " before any engine read");
    }
    g_world_thread = -1;
    {
        const auto saved = api.on_world_thread;
        api.on_world_thread = nullptr;
        ResourceGuardScope owned(n, e.service(), e.reader());
        check(!owned.held() && owned.reason() == Reason::CallerThreadUnknown && words_free(),
              id + "owned with no on_world_thread: refused (fail closed)");
        api.on_world_thread = saved;
    }
    // owned through the scope
    {
        ResourceGuardScope owned(n, e.service(), e.reader());
        check(owned.held() && owned.guard().owns_registry == 1 && *fake::word(e.registry_word()) == 1 && owned.validate() &&
                  owned.contains(e.payload.at(), e.payload.size) && owned.payload() == e.payload.at(),
              id + "owned: held through the table, the registry word Shared, valid, its payload");
        ResourceGuardScope wrong(n, e.service() + 8, e.reader());
        check(!wrong.held() && wrong.result() == KILN_ERR_UNAVAILABLE &&
                  wrong.reason() == Reason::ResourceLifetimeServiceMismatch && *fake::word(e.registry_word()) == 1,
              id + "owned for another service: refused with its reason, the first guard's share kept");
    }
    check(words_free(), id + "owned scopes released");
    // borrowed inside a gate's hold: no KilnApi call and no log from the guard's acquire to its release
    {
        const std::string log_before = g_last_log;
        EcsGateScope gate(n, view, 7, ++g_step);
        const int calls = g_api_calls;
        const std::uint64_t cas = kiln::engine::mem::test_cas.load();
        ResourceGuardScope guard(n, gate, e.service(), e.reader());
        const bool held = guard.held(), valid = guard.validate(), inside = guard.contains(e.payload.at(64), 64);
        guard.release();
        const int section_calls = g_api_calls - calls;
        const bool words_held = *fake::word(e.registry_word()) == 1 &&
                                *fake::word(e.scene_word()) == static_cast<std::int32_t>(0x80000000u);
        check(gate.held() && held && valid && inside && guard.guard().size == sizeof(ResourceGuard),
              id + "borrowed inside the gate's hold: held, valid, contains");
        check(section_calls == 0 && g_last_log == log_before,
              id + "borrowed: no KilnApi call and no log inside the hold (" + std::to_string(section_calls) + ")");
        check(words_held && (p.layout.dedicated_server || kiln::engine::mem::test_cas.load() == cas),
              id + "borrowed: no word of its own; its release leaves the gate holding both");
    }
    check(words_free(), id + "gate and borrowed guard released");
    {
        *fake::word(e.scene_word()) = 1;  // the update's frame: the gate is refused, so is a guard borrowing from it
        EcsGateScope gate(n, view, 7, ++g_step);
        ResourceGuardScope guard(n, gate, e.service(), e.reader());
        check(!gate.held() && !guard.held() && guard.reason() == Reason::ResourceLifetimeGateNotHeld,
              id + "borrowed from a refused gate: resource-lifetime-gate-not-held");
        *fake::word(e.scene_word()) = 0;
    }
    check(words_free(), id + "all guard scopes released");
}

// The engine call through the real table (invoke_test covers every refusal and outcome of the call itself). The
// recipe is the test's, over a code page in the fake image, through the seam the shipped kiln-engine.dll is built
// without; the host's call hook stands in for the engine function.
std::atomic<int> g_call_mode{0};  // 0 returns 0x1234, 1 faults
std::uint64_t g_call_args[8];
kiln::engine::call::Return call_hook(std::uintptr_t, const std::uint64_t* args) {
    std::memcpy(g_call_args, args, sizeof g_call_args);
    if (g_call_mode.load() == 1) return {0, 1, 0xC0000005u};
    return {0x1234, 0, 0};
}
void invoke_members(const KilnEngineNativeApi& n, const Profile& p, fake::Engine& e, const EcsView& view) {
    const std::string id = std::string(p.id) + ": invoke: ";
    const auto words_free = [&] { return *fake::word(e.registry_word()) == 0 && *fake::word(e.scene_word()) == 0; };
    const auto ref = [](Reason r) { return static_cast<std::uint32_t>(r); };
    constexpr std::uint32_t returned = static_cast<std::uint32_t>(CallOutcome::Returned);
    constexpr std::uint32_t uncertain = static_cast<std::uint32_t>(CallOutcome::Uncertain);
    check(n.header.size == sizeof(KilnEngineNativeApi) && n.ecs_gate_invoke && sizeof(KilnEngineNativeApi) == 144 &&
              offsetof(KilnEngineNativeApi, ecs_gate_invoke) == 136,
          id + "the member at 136; the table is 144 bytes");
    kiln::engine::call::test_call_hook = &call_hook;
    g_call_mode = 0;
    const std::size_t page_rva = (p.layout.image_size - 0x10000) & ~std::size_t{0xfff};
    for (std::size_t i = 0; i < 64; ++i) fake::put<std::uint8_t>(e.image.at(page_rva + i), static_cast<std::uint8_t>(i * 13 + 5));
    static CallRecipes recipes{};
    recipes = CallRecipes{};
    recipes.recipe[0].id = 1;
    recipes.recipe[0].arg_count = 2;
    recipes.recipe[0].result_bits = 64;
    recipes.recipe[0].range_count = 1;
    recipes.recipe[0].ranges[0] = {page_rva, 64};
    std::vector<std::uint8_t> captured(64);
    std::memcpy(captured.data(), reinterpret_cast<const void*>(e.image.at(page_rva)), 64);
    KilnEngineCall status{};
    status.size = sizeof status;
    status.op = static_cast<std::uint32_t>(CallOp::Status);
    status.recipe = 1;
    KilnEngineCallResult out{};
    out.size = sizeof out;
    check(n.ecs_gate_invoke(nullptr, &status, &out) == KILN_ERR_UNAVAILABLE &&
              out.reason == ref(Reason::EngineCallRecipeUnavailable) && !p.layout.calls.recipe[0].id,
          id + "the ported profile has no recipe: Status says unavailable");
    gate_invoke_set_test_recipes(&recipes);
    check(n.ecs_gate_invoke(nullptr, &status, &out) == KILN_OK && out.outcome == returned && out.size == sizeof out,
          id + "with the test recipes: Status OK without a gate");
    KilnEngineCall call{};
    call.size = sizeof call;
    call.op = static_cast<std::uint32_t>(CallOp::Invoke);
    call.recipe = 1;
    call.arg_count = 2;
    call.args[0] = 0x11;
    call.args[1] = 0x22;
    call.code = captured.data();
    call.code_bytes = captured.size();
    {
        EcsGate unheld{};
        unheld.size = sizeof unheld;
        EcsGate odd{};
        odd.size = 8;
        KilnEngineCall short_call = call;
        short_call.size = 8;
        KilnEngineCallResult short_out{};
        short_out.size = 8;
        KilnEngineCall unknown = call;
        unknown.op = 3;
        KilnEngineCall latch = call;
        latch.op = static_cast<std::uint32_t>(CallOp::Latch);
        const auto calls = kiln::engine::call::test_calls.load();
        check(n.ecs_gate_invoke(&unheld, nullptr, &out) == KILN_ERR_INVALID_ARGUMENT &&
                  n.ecs_gate_invoke(&unheld, &call, nullptr) == KILN_ERR_INVALID_ARGUMENT &&
                  n.ecs_gate_invoke(&unheld, &short_call, &out) == KILN_ERR_INVALID_ARGUMENT &&
                  n.ecs_gate_invoke(&unheld, &call, &short_out) == KILN_ERR_INVALID_ARGUMENT &&
                  n.ecs_gate_invoke(&unheld, &unknown, &out) == KILN_ERR_INVALID_ARGUMENT &&
                  n.ecs_gate_invoke(nullptr, &call, &out) == KILN_ERR_INVALID_ARGUMENT &&
                  n.ecs_gate_invoke(nullptr, &latch, &out) == KILN_ERR_INVALID_ARGUMENT &&
                  n.ecs_gate_invoke(&odd, &call, &out) == KILN_ERR_INVALID_ARGUMENT,
              id + "NULL or wrong-size records, an unknown op, and Invoke or Latch without a gate record: refused");
        check(n.ecs_gate_invoke(&unheld, &call, &out) == KILN_ERR_UNAVAILABLE && out.outcome == 0 &&
                  out.reason == ref(Reason::EngineCallGateNotHeld) && kiln::engine::call::test_calls.load() == calls &&
                  !gate_calls_latched(),
              id + "an unheld gate: engine-call-gate-not-held, nothing called");
    }
    // a call through the scope: nothing between acquire and release calls the KilnApi or logs
    {
        EcsGateScope scope(n, view, 7, ++g_step);
        const int calls = g_api_calls;
        const std::size_t logs = g_logs.size();
        KilnEngineCallResult r{};
        r.size = sizeof r;
        const KilnResult rc = scope.invoke(call, r);
        const bool held = scope.held();
        const int section_calls = g_api_calls - calls;
        const std::size_t section_logs = g_logs.size() - logs;
        scope.release();
        check(held && rc == KILN_OK && r.outcome == returned && r.value == 0x1234 && g_call_args[0] == 0x11 &&
                  g_call_args[1] == 0x22 && !g_call_args[2],
              id + "called through the scope: returned, the arguments passed");
        check(section_calls == 0 && section_logs == 0 && g_logs.size() == logs,
              id + "no KilnApi call and no log inside the hold, none at the release (" + std::to_string(section_calls) + ")");
    }
    check(words_free(), id + "released");
    // a fault: latched; logged once, after the release, never inside the hold
    g_call_mode = 1;
    {
        EcsGateScope scope(n, view, 7, ++g_step);
        const int calls = g_api_calls;
        const std::size_t logs = g_logs.size();
        KilnEngineCallResult r{};
        r.size = sizeof r;
        const KilnResult rc = scope.invoke(call, r);
        const bool held = scope.held();
        const int section_calls = g_api_calls - calls;
        const std::size_t section_logs = g_logs.size() - logs;
        scope.release();
        check(held && rc == KILN_ERR_UNAVAILABLE && r.outcome == uncertain && r.faulted == 1 &&
                  r.exception == 0xC0000005u && r.reason == ref(Reason::EngineCallFaulted) && gate_calls_latched(),
              id + "a fault: Uncertain, latched; the gate still held until the scope releases it");
        check(section_calls == 0 && section_logs == 0, id + "no KilnApi call and no log inside the hold after a fault");
        const bool one = g_logs.size() == logs + 1 && g_logs.back().first == KILN_LOG_ERROR &&
                         g_logs.back().second.find("engine call 1 ") != std::string::npos &&
                         g_logs.back().second.find("engine-call-faulted") != std::string::npos &&
                         g_logs.back().second.find("0xc0000005") != std::string::npos;
        check(one, id + "logged once at the release, at error level, with the recipe, reason and code: " +
                       (g_logs.size() > logs ? g_logs.back().second : std::string("(none)")));
    }
    const std::size_t after_fault = g_logs.size();
    check(words_free(), id + "released after the fault");
    // the next hold of the same session: refused, nothing called, no new log
    g_call_mode = 0;
    {
        EcsGateScope scope(n, view, 7, ++g_step);
        const auto calls = kiln::engine::call::test_calls.load();
        KilnEngineCallResult r{};
        r.size = sizeof r;
        check(scope.invoke(call, r) == KILN_ERR_UNAVAILABLE && r.reason == ref(Reason::EngineCallLatched) &&
                  kiln::engine::call::test_calls.load() == calls,
              id + "the next hold: engine-call-latched, nothing called");
    }
    check(g_logs.size() == after_fault && n.ecs_gate_invoke(nullptr, &status, &out) == KILN_ERR_UNAVAILABLE &&
              out.reason == ref(Reason::EngineCallLatched),
          id + "no second log line; Status says latched");
    // the server world's next STARTED clears it
    world(KILN_WORLD_STARTED);
    {
        EcsGateScope scope(n, view, 7, ++g_step);
        KilnEngineCallResult r{};
        r.size = sizeof r;
        check(n.ecs_gate_invoke(nullptr, &status, &out) == KILN_OK && scope.invoke(call, r) == KILN_OK && r.value == 0x1234,
              id + "the next STARTED clears the latch: Status OK, the call returns");
        // op Latch through the scope: a member's own check failed
        KilnEngineCall latch = call;
        latch.op = static_cast<std::uint32_t>(CallOp::Latch);
        check(scope.invoke(latch, r) == KILN_OK && gate_calls_latched(), id + "op Latch inside the hold latches");
        const std::size_t logs = g_logs.size();
        scope.release();
        check(g_logs.size() == logs + 1 && g_logs.back().second.find("engine-call-member-uncertain") != std::string::npos,
              id + "and is logged once at the release");
    }
    world(KILN_WORLD_STARTED);
    // the server world stopping while the hold is in place: refused kiln-gate-closed, nothing called
    {
        EcsGateScope scope(n, view, 7, ++g_step);
        std::thread stopping([] { world(KILN_WORLD_STOPPING); });
        stopping.join();
        const auto calls = kiln::engine::call::test_calls.load();
        KilnEngineCallResult r{};
        r.size = sizeof r;
        check(scope.invoke(call, r) == KILN_ERR_UNAVAILABLE && r.reason == ref(Reason::KilnGateClosed) &&
                  kiln::engine::call::test_calls.load() == calls && !gate_calls_latched(),
              id + "STOPPING during the hold: kiln-gate-closed, nothing called, nothing latched");
    }
    world(KILN_WORLD_STARTED);
    check(words_free(), id + "all scopes released");
    gate_invoke_set_test_recipes(nullptr);
    check(n.ecs_gate_invoke(nullptr, &status, &out) == KILN_ERR_UNAVAILABLE &&
              out.reason == ref(Reason::EngineCallRecipeUnavailable),
          id + "without the test recipes: the profile's (none) again");
    kiln::engine::call::test_call_hook = nullptr;
}

// The gate members through the real table over a fake engine (gate_test covers every refusal of the gate itself).
void gate_members(const KilnEngineNativeApi& n, KilnApi& api, const Profile& p, fake::Engine& e, const EcsView& view) {
    const std::string id = std::string(p.id) + ": gate: ";
    const auto words_free = [&] { return *fake::word(e.registry_word()) == 0 && *fake::word(e.scene_word()) == 0; };
    // the caller's thread, from the core, before any word: no engine read, no CAS
    for (const auto& [kind, want] : {std::pair{static_cast<int>(KILN_WORLD_SERVER), Reason::InFrame},
                                     std::pair{static_cast<int>(KILN_WORLD_CLIENT), Reason::WorldThread},
                                     std::pair{static_cast<int>(KILN_WORLD_PREDICTION), Reason::WorldThread}}) {
        g_world_thread = kind;
        EcsGate g{};
        g.size = sizeof g;
        g.owner = 7;
        g.step = ++g_step;
        std::uint32_t reason = 0;
        const std::uint64_t reads = kiln::engine::mem::test_reads.load(), cas = kiln::engine::mem::test_cas.load();
        check(n.ecs_gate_acquire(&view, &g, &reason) == KILN_ERR_UNAVAILABLE && reason == static_cast<std::uint32_t>(want) &&
                  !g.held && kiln::engine::mem::test_reads.load() == reads && kiln::engine::mem::test_cas.load() == cas &&
                  words_free(),
              id + "world kind " + std::to_string(kind) + " refused " + reason_name(want) + " without touching the engine");
    }
    g_world_thread = -1;
    {
        const auto saved = api.on_world_thread;
        api.on_world_thread = nullptr;  // a core that cannot name the thread: fail closed
        EcsGate g{};
        g.size = sizeof g;
        g.owner = 7;
        g.step = ++g_step;
        std::uint32_t reason = 0;
        check(n.ecs_gate_acquire(&view, &g, &reason) == KILN_ERR_UNAVAILABLE &&
                  reason == static_cast<std::uint32_t>(Reason::CallerThreadUnknown) && words_free(),
              id + "no on_world_thread: refused");
        api.on_world_thread = saved;
    }
    // argument checks
    {
        EcsGate g{};
        std::uint32_t reason = 0;
        check(n.ecs_gate_acquire(&view, &g, &reason) == KILN_ERR_INVALID_ARGUMENT, id + "a gate record without its size refused");
        g.size = sizeof g;
        check(n.ecs_gate_acquire(&view, &g, &reason) == KILN_ERR_INVALID_ARGUMENT, id + "no owner and no step refused");
        g.owner = 7;
        check(n.ecs_gate_acquire(&view, &g, &reason) == KILN_ERR_INVALID_ARGUMENT, id + "no step refused");
        g.step = ++g_step;
        g.owner = 0;
        check(n.ecs_gate_acquire(&view, &g, &reason) == KILN_ERR_INVALID_ARGUMENT && words_free(), id + "no owner refused");
        g.owner = 7;
        g.admission = 2;  // neither GateAdmission value
        check(n.ecs_gate_acquire(&view, &g, &reason) == KILN_ERR_INVALID_ARGUMENT && words_free(), id + "unknown admission refused");
        g.admission = 0;
        check(n.ecs_gate_acquire(nullptr, &g, &reason) == KILN_ERR_INVALID_ARGUMENT &&
                  n.ecs_gate_acquire(&view, nullptr, &reason) == KILN_ERR_INVALID_ARGUMENT &&
                  n.ecs_gate_acquire(&view, &g, nullptr) == KILN_ERR_INVALID_ARGUMENT && words_free(),
              id + "NULL arguments refused");
        std::uint64_t v = 1;
        check(n.ecs_gate_write(&g, e.table.at(512), &v, sizeof v) == KILN_ERR_INVALID_ARGUMENT && !n.ecs_gate_validate(&g) &&
                  !n.ecs_gate_validate(nullptr) && !n.ecs_gate_component_supported(nullptr, 3) &&
                  !n.ecs_gate_track_change(nullptr, 0x5000, 3),
              id + "members on a gate that is not held refuse");
        n.ecs_gate_release(nullptr);
        n.ecs_gate_release(&g);
        check(words_free(), id + "release of NULL or an unheld gate is a no-op");
    }
    // a held gate through the RAII scope: nothing between acquire and release calls the KilnApi or logs
    {
        const std::string log_before = g_last_log;
        const int tracker = kiln::engine::mem::test_tracker_calls.load();
        EcsGateScope scope(n, view, 7, ++g_step);
        const int calls = g_api_calls;
        const bool held = scope.held(), valid = scope.validate(), supported = scope.component_supported(3);
        const bool tracked = scope.track_change(0x5000, 3);
        const std::uint64_t payload = 0x55aa55aa55aa55aaull;
        const KilnResult wrote = scope.write(e.table.at(512), &payload, sizeof payload);
        const KilnResult faulted = scope.write(fake::unmapped(), &payload, sizeof payload);
        const KilnResult no_data = scope.write(e.table.at(512), nullptr, 8);
        const bool words_held = *fake::word(e.registry_word()) == 1 &&
                                *fake::word(e.scene_word()) == static_cast<std::int32_t>(0x80000000u);
        scope.release();
        const int section_calls = g_api_calls - calls;
        check(held && valid && supported && tracked && words_held, id + "acquired, valid, component untracked, change recorded");
        check(kiln::engine::mem::test_tracker_calls.load() == tracker + 1, id + "the engine recorder called once");
        check(wrote == KILN_OK && fake::get<std::uint64_t>(e.table.at(512)) == payload && faulted == KILN_ERR_UNAVAILABLE &&
                  no_data == KILN_ERR_INVALID_ARGUMENT,
              id + "guarded write: written, a fault reported, no data refused");
        check(section_calls == 0 && g_last_log == log_before, id + "no KilnApi call and no log inside the hold (" +
                                                                  std::to_string(section_calls) + ")");
        check(words_free() && !scope.held() && scope.write(e.table.at(512), &payload, 8) == KILN_ERR_INVALID_ARGUMENT,
              id + "released: words free, the scope refuses writes");
    }
    // an exception inside the scope still releases
    try {
        EcsGateScope scope(n, view, 7, ++g_step);
        check(scope.held(), id + "held before the throw");
        throw std::runtime_error("member failed");
    } catch (const std::runtime_error&) {
    }
    check(words_free(), id + "an exception inside the scope releases both words");
    // a second writer while one holds it: busy at once
    {
        const std::uint64_t step = ++g_step;
        EcsGateScope first(n, view, 7, step);
        EcsGateScope second(n, view, 8, step);
        check(first.held() && !second.held() && second.reason() == Reason::KilnHolderBusy,
              id + "a second Kiln writer is refused kiln-gate-busy");
        EcsGateScope again(n, view, 8, step);
        check(!again.held() && again.reason() == Reason::KilnBusyLatched, id + "and that owner stays busy for the step");
    }
    check(words_free(), id + "all scopes released");
    // the latch keys on the live server step from the engine's own server Pre subscription
    check(g_tick_fn && g_tick_phase == KILN_TICK_PRE, id + "the engine follows the server step (Pre)");
    {
        server_pre(9000);
        const std::uint64_t stale = 3;  // a caller that never refreshes its step
        EcsGateScope first(n, view, 7, stale);
        EcsGateScope second(n, view, 8, stale);
        check(first.held() && second.reason() == Reason::KilnHolderBusy, id + "live step: a second Kiln writer is busy");
        first.release();
        server_pre(9001);
        EcsGateScope later(n, view, 8, stale);
        check(later.held(), id + "the next server step clears the latch even with a stale caller step");
    }
    check(words_free(), id + "live-step scopes released");
    // a holder in flight at STOPPING is waited for a few microseconds and reported, outside any hold
    {
        EcsGateScope held(n, view, 7, ++g_step);
        g_last_log.clear();
        std::thread stopping([] { world(KILN_WORLD_STOPPING); });
        stopping.join();
        check(held.held() && g_last_log.find("held the engine's write gate at a stopping server world") != std::string::npos,
              id + "STOPPING with a holder in flight says so");
    }
    world(KILN_WORLD_STARTED);
    check(words_free(), id + "STOPPING scope released");
    // the server world's lifecycle closes and reopens the gate; process exit closes it for good
    {
        world(KILN_WORLD_STOPPING);
        EcsGateScope stopping(n, view, 7, ++g_step);
        check(!stopping.held() && stopping.reason() == Reason::KilnGateClosed && words_free(), id + "a stopping world: closed");
    }
    {
        world(KILN_WORLD_STARTED);
        EcsGateScope started(n, view, 7, ++g_step);
        check(started.held(), id + "the next STARTED: open");
    }
    check(g_info.on_unload != nullptr, id + "an exit handler is registered");
    if (g_info.on_unload) {
        {
            EcsGateScope held(n, view, 7, ++g_step);
            g_last_log.clear();
            std::thread exiting([] { g_info.on_unload(g_info.user); });
            exiting.join();
            check(held.held() && g_last_log.find("still held the engine's write gate at exit") != std::string::npos,
                  id + "exit with a holder gives up after its bound and says so");
        }
        EcsGateScope after(n, view, 7, ++g_step);
        check(!after.held() && after.reason() == Reason::KilnGateClosed && words_free(), id + "after exit: closed");
    }
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
    check(t && t->gate_stats(&st) == KILN_OK && st.size == sizeof st, "stats copy (process-wide counters)");
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
    copy = *native; copy.profile_size = sizeof(Profile) - 8;
    check(!kiln_engine_native(&api, 7, sizeof copy, why, sizeof why) && std::strstr(why, "another Kiln build"),
          "profile record size mismatch refused");
    copy = *native; copy.header.version = KILN_ENGINE_NATIVE_VERSION + 1;
    check(!kiln_engine_native(&api, 7, sizeof copy, why, sizeof why), "newer internal version refused");
    copy = *native; copy.header.size = offsetof(KilnEngineNativeApi, layout);
    check(!kiln_engine_native(&api, 7, sizeof(KilnEngineNativeApi), why, sizeof why), "truncated internal table refused");
    copy = *native;
    check(!kiln_engine_native(&api, 7, sizeof copy + 8, why, sizeof why), "a member past the table refused");
    // The typed resourceLayout changed Layout's size: a table from a build before it is refused, so a consumer never
    // reads the profile through a layout of another shape
    static_assert(sizeof(Layout) != 984, "the Layout before the typed resourceLayout was 984 bytes");
    copy = *native; copy.layout_size = 984;
    check(!kiln_engine_native(&api, 7, offsetof(KilnEngineNativeApi, ecs_write_supported), why, sizeof why) &&
              std::strstr(why, "another Kiln build"),
          "a table with the layout size before the typed resourceLayout (984) refused: a mixed build");
    // The engine call appended ecs_gate_invoke (136 -> 144) and the engine-call recipes to the Layout: a table from a build before it
    // is refused for an engine-call consumer, and its layout size for every consumer
    copy = *native; copy.header.size = offsetof(KilnEngineNativeApi, ecs_gate_invoke);  // a table before the engine call (136)
    check(!kiln_engine_native(&api, 7, sizeof(KilnEngineNativeApi), why, sizeof why) &&
              kiln_engine_native(&api, 7, offsetof(KilnEngineNativeApi, ecs_gate_invoke), why, sizeof why) == &copy,
          "a table without the engine-call member: refused for an engine-call consumer, accepted for a guard one");
    copy = *native; copy.layout_size = sizeof(Layout) - sizeof(CallRecipes);
    check(!kiln_engine_native(&api, 7, offsetof(KilnEngineNativeApi, ecs_write_supported), why, sizeof why) &&
              std::strstr(why, "another Kiln build"),
          "a table with the layout size before the engine call refused: a mixed build");
    copy = *native; copy.header.size = offsetof(KilnEngineNativeApi, resource_guard_acquire);  // a table before the resource guard (104)
    check(!kiln_engine_native(&api, 7, sizeof(KilnEngineNativeApi), why, sizeof why) &&
              kiln_engine_native(&api, 7, offsetof(KilnEngineNativeApi, resource_guard_acquire), why, sizeof why) == &copy,
          "a table without the guard members: refused for a guard consumer, accepted for a gate-only one");
    KilnPluginInfo again{}; again.size = sizeof again;
    check(kiln_plugin_main(&api, 8, &again) == KILN_ERR_INVALID_ARGUMENT, "second boot refused");
    g_tables[KILN_ENGINE_NATIVE_SERVICE] = native;
    primitives(*native, api, p);
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
    check(native && !native->layout() && !native->profile(), "no layout and no profile without an exact profile");
    {
        EcsGate g{};
        g.size = sizeof g;
        g.owner = 7;
        g.step = ++g_step;
        EcsView any{};
        std::uint32_t r = 0;
        check(native && native->ecs_gate_acquire(&any, &g, &r) == KILN_ERR_UNAVAILABLE &&
                  r == static_cast<std::uint32_t>(Reason::EcsWriteGateDisabled) && !g.held,
              "no gate on an unknown build");
    }
    EcsView view{};
    std::uint32_t reason = 0;
    check(native && native->ecs_view(0, &view, &reason) == KILN_ERR_UNAVAILABLE &&
              reason == static_cast<std::uint32_t>(Reason::EcsReadOwnerProfileUnavailable) &&
              native->ecs_write_supported(&view, 3) == 0,
          "views and write support refused on an unknown build");
    std::uint64_t value = 7, out = 0;
    check(native && native->copy(reinterpret_cast<std::uintptr_t>(&value), &out, sizeof out) == KILN_OK && out == 7,
          "copy needs no profile");
}

// The measurement switch for the gate's priority raise (<data>/kiln.engine/gate-priority-raise), read once at boot:
// no file leaves the raise on and says nothing; exactly "off" (whitespace and a UTF-8 byte-order mark trimmed) turns
// it off for the process, with a warning and the row unavailable for policy; any other content, or a file that cannot
// be read, leaves it on with a warning. Off, a held gate leaves the holder's priority as it was.
size_t warnings_with(const std::string& part) {
    size_t n = 0;
    for (const auto& [level, text] : g_logs)
        if (level == KILN_LOG_WARN && text.find(part) != std::string::npos) ++n;
    return n;
}
void raise_switch(const Profile& p) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("kiln-engine-switch-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "kiln.engine");
    g_data_dir = root.string();
    const fs::path file = root / "kiln.engine" / "gate-priority-raise";
    struct Case { const char* what; const char* content; bool directory; bool raise; const char* warn; };
    const Case cases[] = {
        {"no file", nullptr, false, true, nullptr},
        {"\"off\"", "off", false, false, "priority raise is off for this run"},
        {"\"off\" with whitespace and a byte-order mark", "\xEF\xBB\xBF  off \r\n", false, false, "priority raise is off for this run"},
        {"\"OFF\"", "OFF", false, true, "holds something other than \"off\""},
        {"\"on\"", "on", false, true, "holds something other than \"off\""},
        {"\"off please\"", "off please", false, true, "holds something other than \"off\""},
        {"an empty file", "", false, true, "holds something other than \"off\""},
        {"a long file starting with off", "off                                                                    x", false,
         true, "holds something other than \"off\""},
        {"a folder of that name", nullptr, true, true, "could not be read"},
    };
    for (const Case& c : cases) {
        fs::remove_all(file);
        if (c.directory) fs::create_directories(file);
        else if (c.content) std::ofstream(file, std::ios::binary) << c.content;
        fresh();
        g_facts = facts_of(p);
        KilnApi api = make_api();
        const std::string id = std::string(p.id) + ": the raise switch, " + c.what + ": ";
        check(boot(api) == KILN_OK, id + "boot");
        check(gate_priority_raise() == c.raise, id + (c.raise ? "the raise stays on" : "the raise is off"));
        const Row& row = g_rows["engine.gate_priority_raise"];
        check(g_rows.count("engine.gate_priority_raise") &&
                  row.state == (c.raise ? KILN_CAP_AVAILABLE : KILN_CAP_UNAVAILABLE) &&
                  row.reason == (c.raise ? KILN_CAP_REASON_NONE : KILN_CAP_REASON_POLICY) &&
                  (c.warn ? !row.text.empty() : row.text.empty()),
              id + "the capability row says so: " + row.text);
        check(c.warn ? warnings_with(c.warn) == 1 : warnings_with("gate-priority-raise") == 0,
              id + (c.warn ? std::string("one warning: ") + c.warn : "no warning"));
        // a held gate: raised only while the switch leaves the raise on
        fake::Engine e(p.layout);
        kiln::engine::mem::test_image = e.image.at();
        char why[160]{};
        const KilnEngineNativeApi* n = kiln_engine_native(&api, 7, sizeof(KilnEngineNativeApi), why, sizeof why);
        EcsView view{};
        std::uint32_t reason = 0;
        if (n && n->ecs_view(0, &view, &reason) == KILN_OK) {
            kiln::engine::mem::test_priority = -1;
            const int boosts = kiln::engine::mem::test_boosts;
            {
                EcsGateScope scope(*n, view, 7, ++g_step);
                check(scope.held() && kiln::engine::mem::test_priority == (c.raise ? 1 : -1),
                      id + (c.raise ? "a holder runs above normal" : "a holder keeps its own priority"));
            }
            check(kiln::engine::mem::test_priority == -1 && kiln::engine::mem::test_boosts == boosts + (c.raise ? 1 : 0),
                  id + "the priority is back after the release");
        } else {
            check(false, id + "a view for the gate");
        }
        kiln::engine::mem::test_image = 0;
    }
    fs::remove_all(root);
    g_data_dir.clear();
}

// The dialect owner: kiln.engine resolves kiln.internal.launch's dialect_owner once, at its init, through the
// services' KilnApi; the gate reads it at each acquire, because the core designates the owner later (when it loads the
// plugin, after every service). The designated owner is never latched; everyone else keeps the latch. An absent, older,
// newer or malformed table leaves the gate exactly as on main.
std::atomic<KilnOwner> g_dialect{0};
KilnOwner KILN_CALL dialect_owner_fn() { return g_dialect.load(std::memory_order_acquire); }
struct DialectRun {
    bool booted = false, viewed = false;
    int lookups_at_init = -1, lookups_after = -1;
    bool busy_first = false;   // both candidates refused kiln-gate-busy while a third owner held the gate
    bool admission_kept = false;  // the records' admission is still the caller's (KilnMod)
    bool spared = false;       // the designated candidate was admitted again in the same step
    bool other_latched = false;  // the other owner got kiln-gate-busy-this-step
    size_t logs = 0;
};
constexpr KilnOwner kDialectCandidate = 30, kOtherOwner = 31;
// One boot with `table` under kiln.internal.launch (nullptr: the core has none), then `designate` as the core does when
// it loads the plugin, then one step of the gate: a holder, the candidate and another owner busy, then both again.
DialectRun dialect_run(const Profile& p, const void* table, KilnOwner designate) {
    fresh();
    g_facts = facts_of(p);
    g_dialect = 0;
    if (table) g_tables[KILN_LAUNCH_INTERNAL_SERVICE] = table;
    KilnApi api = make_api();
    DialectRun r;
    r.booted = boot(api) == KILN_OK;
    r.lookups_at_init = g_lookups[KILN_LAUNCH_INTERNAL_SERVICE];
    r.logs = g_logs.size();
    g_dialect = designate;
    fake::Engine e(p.layout);
    kiln::engine::mem::test_image = e.image.at();
    char why[160]{};
    const KilnEngineNativeApi* n = kiln_engine_native(&api, 7, sizeof(KilnEngineNativeApi), why, sizeof why);
    EcsView view{};
    std::uint32_t reason = 0;
    r.viewed = n && n->ecs_view(0, &view, &reason) == KILN_OK;
    if (r.viewed) {
        const std::uint64_t step = ++g_step;
        {
            EcsGateScope holder(*n, view, 7, step);
            EcsGateScope candidate(*n, view, kDialectCandidate, step);
            EcsGateScope other(*n, view, kOtherOwner, step);
            r.busy_first = holder.held() && candidate.reason() == Reason::KilnHolderBusy &&
                           other.reason() == Reason::KilnHolderBusy;
            r.admission_kept = candidate.gate().admission == static_cast<std::uint32_t>(GateAdmission::KilnMod) &&
                               other.gate().admission == static_cast<std::uint32_t>(GateAdmission::KilnMod);
        }
        {
            EcsGateScope candidate(*n, view, kDialectCandidate, step);
            r.spared = candidate.held();
        }
        EcsGateScope other(*n, view, kOtherOwner, step);
        r.other_latched = !other.held() && other.reason() == Reason::KilnBusyLatched;
    }
    r.lookups_after = g_lookups[KILN_LAUNCH_INTERNAL_SERVICE];
    check(*fake::word(e.registry_word()) == 0 && *fake::word(e.scene_word()) == 0,
          std::string(p.id) + ": dialect owner: words free after the run");
    kiln::engine::mem::test_image = 0;
    g_dialect = 0;
    return r;
}
void dialect_owner(const Profile& p) {
    const std::string id = std::string(p.id) + ": dialect owner: ";
    const KilnLaunchInternalApi good = {{sizeof(KilnLaunchInternalApi), KILN_LAUNCH_INTERNAL_VERSION}, &dialect_owner_fn};
    // designated after kiln.engine's init: never latched; its refusal by another Kiln holder was retried (gate_test
    // counts the attempts); another owner in the same step is latched
    DialectRun d = dialect_run(p, &good, kDialectCandidate);
    check(d.booted && d.viewed && d.busy_first && d.admission_kept, id + "designated: busy behind another Kiln holder, "
                                                                         "the records' admission left as the caller set it");
    check(d.spared, id + "designated after init: admitted again in the same step (not latched)");
    check(d.other_latched, id + "another owner in the same step gets kiln-gate-busy-this-step");
    check(d.lookups_at_init == 1 && d.lookups_after == 1,
          id + "kiln.internal.launch is looked up once, at init, never per acquire (" + std::to_string(d.lookups_at_init) +
              " at init, " + std::to_string(d.lookups_after) + " after)");
    // main's gate, with the rule's lookup the only difference
    DialectRun absent = dialect_run(p, nullptr, kDialectCandidate);
    check(absent.booted && absent.viewed && absent.busy_first && !absent.spared && absent.other_latched &&
              absent.lookups_at_init == 1 && absent.lookups_after == 1,
          id + "no kiln.internal.launch (an older core): every owner latched, as on main");
    check(absent.logs == d.logs, id + "no log line either way (" + std::to_string(absent.logs) + " and " +
                                     std::to_string(d.logs) + " lines at boot)");
    DialectRun nobody = dialect_run(p, &good, 0);
    check(nobody.viewed && !nobody.spared && nobody.other_latched, id + "nobody designated: every owner latched");
    DialectRun another = dialect_run(p, &good, kDialectCandidate + 100);
    check(another.viewed && !another.spared && another.other_latched, id + "another owner designated: the candidate is latched");
    KilnLaunchInternalApi older = good;
    older.header.version = 0;
    DialectRun o = dialect_run(p, &older, kDialectCandidate);
    check(o.viewed && !o.spared && o.other_latched, id + "an older table (version 0): ignored, main's gate");
    KilnLaunchInternalApi newer = good;
    newer.header.version = KILN_LAUNCH_INTERNAL_VERSION + 1;
    DialectRun nw = dialect_run(p, &newer, kDialectCandidate);
    check(nw.viewed && !nw.spared && nw.other_latched, id + "a table of another version: ignored, main's gate");
    KilnLaunchInternalApi truncated = good;
    truncated.header.size = offsetof(KilnLaunchInternalApi, dialect_owner);
    DialectRun tr = dialect_run(p, &truncated, kDialectCandidate);
    check(tr.viewed && !tr.spared && tr.other_latched, id + "a table too short for dialect_owner: ignored, main's gate");
    KilnLaunchInternalApi empty = good;
    empty.dialect_owner = nullptr;
    DialectRun em = dialect_run(p, &empty, kDialectCandidate);
    check(em.viewed && !em.spared && em.other_latched, id + "a table without dialect_owner: ignored, no call, main's gate");
    // a later boot without the table forgets the accessor of the earlier one
    dialect_run(p, &good, kDialectCandidate);
    DialectRun after = dialect_run(p, nullptr, kDialectCandidate);
    check(!after.spared && after.other_latched, id + "the next boot without the table: main's gate again");
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
    for (const auto& p : generated::kProfiles) dialect_owner(p);
    unknown_build();
    raise_switch(generated::kProfiles[0]);
    internal_refused();
    reset_for_tests();
    std::printf("engine plugin host: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
