// SPDX-License-Identifier: MIT
// kiln.repl.net on a JOINED game client (from package 15 review repro repl_review_client_test.cpp) (no server world), built with the service's
// own sources like tests/repl_client_test.cpp (same stand-in kiln.hooks). Two cases:
//  A. a receive track (SidecarClient's) without any receive watch: does a create the client stores reach the track?
//     client.cpp store_before returns on '!g_hot_n' (no watch) before track_client_store -> expected 0 events (bug).
//  B. control: the same with one receive watch beside it -> the create arrives.
//  C. client receive message hooks: messages_pre only runs from on_pre with a server world, so a removed hook's slot is
//     never freed on a joined client: register + remove 70 times -> the 65th registration fails (bug).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "kiln_api.h"
#include "services/kiln_caps.h"
#include "services/kiln_hooks.h"
#include "services/kiln_repl.h"

extern "C" KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info);
namespace repl {  // track.cpp's receive side (repl.h), called directly by case D to isolate the store hook
bool track_client_active();
void track_client_store(const uint8_t* msg, uint64_t size);
}

namespace {
int g_checks = 0, g_failed = 0;
const char* g_case = "";
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL [%s] %s\n", g_case, what);
    }
}
#define CHECK(x) check((x), #x)

struct Slice {
    const uint8_t* ptr;
    uint64_t size;
};
using StoreFn = void (*)(uintptr_t world, const Slice* msg, uint64_t pred);

const KilnReplApi* g_table = nullptr;
const KilnReplNetApi* g_net = nullptr;
uint64_t g_links = 0;
std::map<std::string, KilnHandle> g_live;
std::map<KilnHandle, std::string> g_label;
void* g_store_detour = nullptr;
KilnTickFn g_client_pre = nullptr;

void store_original(uintptr_t, const Slice*, uint64_t) {}
void decode_original() {}
// distinct addresses (three empty functions would be folded into one by the linker's identical-code folding, and the
// stand-in kiln.hooks below tells the store hook apart from the others by its target)
char dummy_store_sym, dummy_decode_sym, dummy_other_sym;
#define dummy_store dummy_store_sym
#define dummy_decode dummy_decode_sym
#define dummy_other dummy_other_sym

void KILN_CALL api_log(KilnOwner, int32_t, KilnStr s) { std::printf("  log: %.*s\n", (int)s.len, s.ptr); }
bool KILN_CALL api_feature(KilnStr name) {
    const std::string n(name.ptr, name.len);
    return n == "repl.client" || n == "repl.messages.client";
}
KilnResult KILN_CALL api_path(int32_t, char*, size_t, size_t*) { return KILN_ERR_INVALID_ARGUMENT; }
KilnResult KILN_CALL api_symbol(KilnStr name, uintptr_t* out) {
    const std::string n(name.ptr, name.len);
    *out = n == "repl/ClientStore" ? reinterpret_cast<uintptr_t>(&dummy_store)
         : n == "repl/Decode"      ? reinterpret_cast<uintptr_t>(&dummy_decode)
                                   : reinterpret_cast<uintptr_t>(&dummy_other);
    return KILN_OK;
}
KilnResult KILN_CALL api_constant(KilnStr name, int64_t* out) {
    if (std::string(name.ptr, name.len) != "world.storage") return KILN_ERR_NOT_FOUND;
    *out = 0xe179c0;
    return KILN_OK;
}
KilnResult KILN_CALL caps_find(KilnStr, KilnCapRow*) { return KILN_ERR_NOT_FOUND; }
KilnResult KILN_CALL caps_report(KilnOwner, KilnStr, uint32_t, uint32_t, KilnStr) { return KILN_OK; }
KilnResult KILN_CALL hooks_raw_detour(KilnOwner, uintptr_t target, const KilnHookOrder* order, void* fn, void** original,
                                      KilnHandle* out) {
    const std::string label = order ? std::string(order->label.ptr, order->label.len) : std::string();
    if (target == reinterpret_cast<uintptr_t>(&dummy_store)) {
        g_store_detour = fn;
        if (original) *original = reinterpret_cast<void*>(&store_original);
    } else if (original) {
        *original = reinterpret_cast<void*>(&decode_original);
    }
    *out = ++g_links;
    g_live[label] = *out;
    g_label[*out] = label;
    return KILN_OK;
}
KilnResult KILN_CALL hooks_mid2(KilnOwner, uintptr_t, const KilnHookOrder* order, KilnMidFn, void*, KilnHandle* out) {
    const std::string label = order ? std::string(order->label.ptr, order->label.len) : std::string();
    *out = ++g_links;
    g_live[label] = *out;
    g_label[*out] = label;
    return KILN_OK;
}
KilnResult KILN_CALL hooks_revoke(KilnHandle h) {
    auto it = g_label.find(h);
    if (it != g_label.end()) {
        if (g_live[it->second] == h) g_live.erase(it->second);
        g_label.erase(it);
    }
    return KILN_OK;
}
KilnResult KILN_CALL hooks_subscribe_retiring(KilnOwner, KilnOwnerRetiringFn, void*, KilnHandle* out) {
    *out = 77;
    return KILN_OK;
}
const void* KILN_CALL api_service(KilnStr name, uint32_t) {
    static KilnCapsApi caps = [] {
        KilnCapsApi t{};
        t.header = {sizeof t, KILN_CAPS_VERSION};
        t.find = &caps_find;
        t.report = &caps_report;
        return t;
    }();
    static KilnHooksApi hooks = [] {
        KilnHooksApi t{};
        t.header = {sizeof t, KILN_HOOKS_VERSION};
        t.raw_detour = &hooks_raw_detour;
        t.mid2 = &hooks_mid2;
        t.revoke = &hooks_revoke;
        t.subscribe_owner_retiring = &hooks_subscribe_retiring;
        return t;
    }();
    const std::string n(name.ptr, name.len);
    if (n == KILN_CAPS_SERVICE) return &caps;
    if (n == KILN_HOOKS_SERVICE) return &hooks;
    return nullptr;
}
KilnResult KILN_CALL api_register(KilnOwner, KilnStr name, const void* table) {
    const std::string n(name.ptr, name.len);
    if (n == KILN_REPL_SERVICE) g_table = static_cast<const KilnReplApi*>(table);
    if (n == KILN_REPL_NET_SERVICE) g_net = static_cast<const KilnReplNetApi*>(table);
    return KILN_OK;
}
KilnTickFn g_server_pre = nullptr;
KilnResult KILN_CALL api_subscribe_tick(KilnOwner, int32_t, KilnTickFn fn, void*, KilnHandle* out) {
    g_server_pre = fn;
    *out = 1;
    return KILN_OK;
}
KilnResult KILN_CALL api_subscribe_world(KilnOwner, KilnWorldEventFn, void*, KilnHandle* out) {
    *out = 2;
    return KILN_OK;
}
KilnResult KILN_CALL api_subscribe_world_tick(KilnOwner, uint32_t kind, int32_t, KilnTickFn fn, void*, KilnHandle* out) {
    if (kind == KILN_WORLD_CLIENT) g_client_pre = fn;
    *out = 3;
    return KILN_OK;
}
bool KILN_CALL api_on_tick_thread() { return false; }  // a joined client: no server world
bool KILN_CALL api_on_world_thread(uint32_t kind) { return kind == KILN_WORLD_CLIENT; }
uintptr_t KILN_CALL api_server_world() { return 0; }
uintptr_t KILN_CALL api_world(uint32_t) { return 0; }
uint32_t KILN_CALL api_side() { return KILN_SIDE_CLIENT; }

KilnApi make_api() {
    KilnApi a{};
    a.size = sizeof a;
    a.abi_version = KILN_ABI_VERSION;
    a.log = &api_log;
    a.feature = &api_feature;
    a.path = &api_path;
    a.symbol = &api_symbol;
    a.constant = &api_constant;
    a.service = &api_service;
    a.register_service = &api_register;
    a.subscribe_tick = &api_subscribe_tick;
    a.subscribe_world = &api_subscribe_world;
    a.subscribe_world_tick = &api_subscribe_world_tick;
    a.on_tick_thread = &api_on_tick_thread;
    a.on_world_thread = &api_on_world_thread;
    a.server_world = &api_server_world;
    a.world = &api_world;
    a.side = &api_side;
    return a;
}

std::vector<uint8_t> create_msg(uint32_t tick, uint32_t seq, uint32_t entity) {
    std::vector<uint8_t> m(12);
    const uint32_t w0 = (tick << 3) | 2;
    std::memcpy(m.data(), &w0, 4);
    std::memcpy(m.data() + 4, &seq, 4);
    std::memcpy(m.data() + 8, &entity, 4);
    m.resize(12 + 4 + 16 + 1, 0);
    m.push_back(4);
    m.push_back(0);
    m.resize(m.size() + 4, 0x5A);
    return m;
}
void store(const std::vector<uint8_t>& m) {
    const Slice s{m.data(), m.size()};
    if (g_store_detour) reinterpret_cast<StoreFn>(g_store_detour)(0, &s, 0);
}
void pre() {
    KilnTickInfo t{};
    t.size = sizeof t;
    t.world_kind = KILN_WORLD_CLIENT;
    if (g_client_pre) g_client_pre(&t, nullptr);
}
uint32_t g_created = 0;
void KILN_CALL on_track(const KilnReplTrackEvent* e, uint32_t n, void*) {
    for (uint32_t i = 0; i < n; ++i)
        if (e[i].kind == KILN_REPL_TRACK_CREATED) ++g_created;
}
void KILN_CALL on_event(const KilnReplWatchEvent*, void*) {}
int32_t KILN_CALL on_msg(const KilnReplMessage*, void*) { return KILN_REPL_PASS; }
}  // namespace

int main() {
    static const KilnApi api = make_api();
    KilnPluginInfo info{};
    info.size = sizeof info;
    g_case = "start";
    CHECK(kiln_plugin_main(&api, 7, &info) == KILN_OK && g_table && g_net);
    if (!g_table || !g_net) return 1;
    CHECK((g_net->capabilities() & KILN_REPL_NET_CAP_CLIENT_TRACK) != 0);

    g_case = "B0: control first: a receive watch, then a receive track";
    {
        KilnHandle w0 = 0, t0 = 0;
        CHECK(g_table->watch(8, 999, KILN_REPL_ALL_MACHINES, KILN_REPL_RECEIVE, &on_event, nullptr, &w0) == KILN_OK);
        CHECK(g_net->track(9, KILN_REPL_RECEIVE, &on_track, nullptr, &t0) == KILN_OK);
        store(create_msg(400, 1, 4240));
        pre();
        std::printf("  B0: CREATED events delivered = %u (expected 1)\n", g_created);
        CHECK(g_created == 1);
        g_table->remove(w0);
        g_net->remove(t0);
        pre();
        g_created = 0;
    }
    g_case = "A: receive track, no receive watch (SidecarClient's case)";
    KilnHandle tr = 0;
    CHECK(g_net->track(9, KILN_REPL_RECEIVE, &on_track, nullptr, &tr) == KILN_OK && tr != 0);
    CHECK(g_live.count("repl/client ClientStore") == 1);  // the store hook went in for the track
    store(create_msg(500, 1, 4242));
    pre();
    std::printf("  A: CREATED events delivered = %u (expected 1)\n", g_created);
    CHECK(g_created == 1);

    g_case = "B: control, the same with a receive watch beside it";
    g_created = 0;
    KilnHandle w = 0;
    CHECK(g_table->watch(8, 999, KILN_REPL_ALL_MACHINES, KILN_REPL_RECEIVE, &on_event, nullptr, &w) == KILN_OK);
    CHECK(g_table->stats_enable(5, true) == KILN_OK);  // the receive counters: did the store hook see the message at all?
    { const auto m = create_msg(501, 2, 4243); const Slice s{m.data(), m.size()};
      std::printf("  B: store detour %p\n", g_store_detour); reinterpret_cast<StoreFn>(g_store_detour)(0, &s, 0); }
    {
        std::vector<KilnReplPeerStats> rows(4);
        for (auto& r : rows) r.size = sizeof r;
        uint32_t n = 0;
        const KilnResult rc = g_table->peer_stats(rows.data(), 4, &n);
        std::printf("  B: peer_stats rc %d rows %u; ch4 messages %llu creates %llu\n", rc, n, n ? (unsigned long long)rows[0].messages : 0ull,
                    n ? (unsigned long long)rows[0].creates : 0ull);
    }
    pre();
    if (g_server_pre) { KilnTickInfo t{}; t.size = sizeof t; g_server_pre(&t, nullptr); }  // flushes kiln.repl's guard-fault lines
    std::printf("  B: CREATED events delivered = %u (expected 1)\n", g_created);
    CHECK(g_created == 1);

    g_case = "D: isolate: track_client_store called directly (bypassing client.cpp's store hook)";
    g_created = 0;
    std::printf("  D: track_client_active() = %d\n", repl::track_client_active() ? 1 : 0);
    { const auto m = create_msg(502, 3, 4244); repl::track_client_store(m.data(), m.size()); }
    pre();
    std::printf("  D: CREATED events delivered = %u (expected 1)\n", g_created);
    CHECK(g_created == 1);
    g_table->remove(w);
    g_net->remove(tr);
    pre();

    g_case = "C: client receive message hooks on a joined client, register + remove 70 times";
    CHECK((g_net->capabilities() & KILN_REPL_NET_CAP_MESSAGES_RECEIVE) != 0);
    int ok = 0, first_fail = -1;
    for (int i = 0; i < 70; ++i) {
        KilnHandle h = 0;
        const KilnResult rc = g_net->message_hook(11, 19, KILN_REPL_RECEIVE, &on_msg, nullptr, &h);
        if (rc == KILN_OK) {
            ++ok;
            g_net->remove(h);
        } else if (first_fail < 0) {
            first_fail = i;
        }
        pre();  // the client world's Pre (the only Pre a joined client has)
    }
    std::printf("  C: %d of 70 registrations succeeded; first refusal at #%d; mid hooks still in: %zu\n", ok, first_fail + 1,
                g_live.count("repl/messages client machine") + g_live.count("repl/messages client player"));
    CHECK(ok == 70);
    CHECK(g_live.count("repl/messages client machine") == 0);  // no hook left: the receive sites should be out

    std::printf("repl review client repro: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
