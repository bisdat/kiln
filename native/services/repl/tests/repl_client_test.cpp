// SPDX-License-Identifier: MIT
// kiln.repl on a game client (no game; built with the service's own sources, run under plain Wine by tests/run.sh): the
// client's own receive counters. stats_enable / peer_stats called on the client world's thread hold and read what this
// client receives: the test calls the message-store hook kiln.repl installed (a stand-in kiln.hooks hands it back) with
// hand-made messages of both channels and reads the rows; the hook goes with the last holder or its owner's revoke;
// a refused hook answers KILN_ERR_UNAVAILABLE; repl.observe is off here (a joined client), the counters need only
// repl.client.
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
bool g_main = true;  // the test runs "on the client world's thread"
std::string g_fail;
uint64_t g_links = 0;
std::map<std::string, KilnHandle> g_live;  // label -> handle of a live link
std::map<KilnHandle, std::string> g_label;
void* g_store_detour = nullptr;
int g_store_originals = 0;
KilnTickFn g_client_pre = nullptr;
KilnOwnerRetiringFn g_retiring = nullptr;

void store_original(uintptr_t, const Slice*, uint64_t) { ++g_store_originals; }
void decode_original() {}
void dummy_store() {}
void dummy_decode() {}
void dummy_other() {}

void KILN_CALL api_log(KilnOwner, int32_t, KilnStr) {}
bool KILN_CALL api_feature(KilnStr name) { return std::string(name.ptr, name.len) == "repl.client"; }
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
    if (!g_fail.empty() && label.find(g_fail) != std::string::npos) return KILN_ERR_UNAVAILABLE;
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
KilnResult KILN_CALL hooks_revoke(KilnHandle h) {
    auto it = g_label.find(h);
    if (it != g_label.end()) {
        if (g_live[it->second] == h) g_live.erase(it->second);
        g_label.erase(it);
    }
    return KILN_OK;
}
KilnResult KILN_CALL hooks_subscribe_retiring(KilnOwner, KilnOwnerRetiringFn fn, void*, KilnHandle* out) {
    g_retiring = fn;
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
    if (std::string(name.ptr, name.len) == KILN_REPL_SERVICE) g_table = static_cast<const KilnReplApi*>(table);
    return KILN_OK;
}
KilnResult KILN_CALL api_subscribe_tick(KilnOwner, int32_t, KilnTickFn, void*, KilnHandle* out) {
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
bool KILN_CALL api_on_tick_thread() { return false; }  // no server world here: a joined client
bool KILN_CALL api_on_world_thread(uint32_t kind) { return kind == KILN_WORLD_CLIENT && g_main; }
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

std::vector<uint8_t> entity_msg(uint32_t kind, uint32_t tick, uint32_t seq, uint32_t entity, uint16_t payload) {
    std::vector<uint8_t> m(12);
    const uint32_t w0 = (tick << 3) | kind;
    std::memcpy(m.data(), &w0, 4);
    std::memcpy(m.data() + 4, &seq, 4);
    std::memcpy(m.data() + 8, &entity, 4);
    if (kind == 2) {
        m.resize(12 + 4 + 16 + 1, 0);
        m[16] = 0xAB;
    } else {
        const uint32_t base = 0x80000000u | (tick - 4);
        m.resize(16);
        std::memcpy(m.data() + 12, &base, 4);
    }
    m.push_back(static_cast<uint8_t>(payload));
    m.push_back(static_cast<uint8_t>(payload >> 8));
    m.resize(m.size() + payload, 0x5A);
    return m;
}
std::vector<uint8_t> destroy_msg(uint32_t tick, uint32_t seq, std::vector<uint32_t> a, std::vector<uint32_t> b) {
    std::vector<uint8_t> m(10);
    const uint32_t w0 = (tick << 3) | 4;
    std::memcpy(m.data(), &w0, 4);
    std::memcpy(m.data() + 4, &seq, 4);
    m[8] = static_cast<uint8_t>(a.size());
    m[9] = static_cast<uint8_t>(b.size());
    for (auto* l : {&a, &b})
        for (uint32_t id : *l) {
            m.resize(m.size() + 4);
            std::memcpy(m.data() + m.size() - 4, &id, 4);
        }
    return m;
}

void store(const std::vector<uint8_t>& m, uint64_t pred) {
    const Slice s{m.data(), m.size()};
    if (g_store_detour) reinterpret_cast<StoreFn>(g_store_detour)(0, &s, pred);
}

const KilnReplPeerStats* row(const std::vector<KilnReplPeerStats>& rows, uint8_t ch) {
    for (const auto& r : rows)
        if (r.channel == ch) return &r;
    return nullptr;
}
std::vector<KilnReplPeerStats> rows_now(KilnResult* rc) {
    std::vector<KilnReplPeerStats> rows(4);
    for (auto& r : rows) r.size = sizeof r;
    uint32_t n = 0;
    *rc = g_table->peer_stats(rows.data(), static_cast<uint32_t>(rows.size()), &n);
    rows.resize(n < rows.size() ? n : rows.size());
    return rows;
}

void pre() {
    KilnTickInfo t{};
    t.size = sizeof t;
    t.world_kind = KILN_WORLD_CLIENT;
    if (g_client_pre) g_client_pre(&t, nullptr);
}

void KILN_CALL on_event(const KilnReplWatchEvent*, void*) {}

}  // namespace

int main() {
    static const KilnApi api = make_api();
    KilnPluginInfo info{};
    info.size = sizeof info;
    g_case = "start";
    CHECK(kiln_plugin_main(&api, 7, &info) == KILN_OK && g_table);
    if (!g_table) return 1;
    CHECK((g_table->capabilities() & KILN_REPL_CAP_CLIENT) != 0 && (g_table->capabilities() & KILN_REPL_CAP_OBSERVE) == 0);
    CHECK(g_client_pre != nullptr && g_retiring != nullptr);
    CHECK(g_live.empty());  // nothing hooked before anyone asks

    g_case = "counters";
    KilnResult rc = KILN_ERR_INTERNAL;
    CHECK(rows_now(&rc).empty() && rc == KILN_OK);
    CHECK(g_table->stats_enable(5, true) == KILN_OK);
    CHECK(g_live.count("repl/client ClientStore") == 1 && g_live.count("repl/client Decode") == 0);  // the store only
    const auto c1 = entity_msg(2, 100, 1, 2542, 9), u1 = entity_msg(3, 100, 2, 2542, 5), u2 = entity_msg(3, 101, 3, 2542, 4);
    const auto d1 = destroy_msg(101, 4, {7}, {8, 9}), p1 = entity_msg(3, 101, 1, 1, 6);
    store(c1, 0);
    store(u1, 0);
    store(u2, 0);
    store(d1, 0);
    store(p1, 1);
    CHECK(g_store_originals == 5);  // the game's store ran once per message
    auto rows = rows_now(&rc);
    CHECK(rc == KILN_OK && rows.size() == 2);
    const KilnReplPeerStats* r4 = row(rows, 4);
    const KilnReplPeerStats* r5 = row(rows, 5);
    CHECK(r4 && r4->flags == KILN_REPL_PEER_RECEIVED && r4->peer == 0 && r4->frames == 2 && r4->messages == 4 &&
          r4->bytes == c1.size() + u1.size() + u2.size() + d1.size() && r4->creates == 1 && r4->updates == 2 &&
          r4->destroy_lists == 1 && r4->destroyed_ids == 3 && r4->unchanged == 0 && r4->acks == 0);
    CHECK(r5 && r5->flags == KILN_REPL_PEER_RECEIVED && r5->frames == 1 && r5->messages == 1 && r5->updates == 1 &&
          r5->bytes == p1.size());
    uint32_t need = 0;
    CHECK(g_table->peer_stats(nullptr, 0, &need) == KILN_ERR_BUFFER_TOO_SMALL && need == 2);

    g_case = "holders";
    CHECK(g_table->stats_enable(6, true) == KILN_OK);  // a second holder: the counters go on (no reset)
    rows = rows_now(&rc);
    CHECK(row(rows, 4) && row(rows, 4)->messages == 4);
    CHECK(g_table->stats_enable(5, false) == KILN_OK && g_live.count("repl/client ClientStore") == 1);
    if (g_retiring) g_retiring(6, nullptr);  // the last holder's owner goes: the hook goes at the next client Pre
    pre();
    CHECK(g_live.count("repl/client ClientStore") == 0);
    CHECK(g_table->stats_enable(5, true) == KILN_OK);  // a first holder again: from 0
    rows = rows_now(&rc);
    CHECK(rows.empty());
    CHECK(g_table->stats_enable(5, false) == KILN_OK && g_live.empty());

    g_case = "refused and threads";
    g_fail = "ClientStore";
    CHECK(g_table->stats_enable(9, true) == KILN_ERR_UNAVAILABLE && g_live.empty());
    g_fail.clear();
    g_main = false;  // off the client world's thread (and no server world): not the receive counters
    CHECK(g_table->stats_enable(9, true) != KILN_OK);
    g_main = true;
    CHECK(g_table->stats_enable(0, true) == KILN_ERR_INVALID_ARGUMENT);

    g_case = "receive watch beside";
    KilnHandle h = 0;
    CHECK(g_table->watch(8, 42, KILN_REPL_ALL_MACHINES, KILN_REPL_RECEIVE, &on_event, nullptr, &h) == KILN_OK && h != 0);
    CHECK(g_live.count("repl/client ClientStore") == 1 && g_live.count("repl/client Decode") == 1);
    CHECK(g_table->remove(h) == KILN_OK);
    pre();
    CHECK(g_live.empty());

    std::printf("repl client tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
