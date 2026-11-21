// SPDX-License-Identifier: MIT
// kiln.repl's watch and stats bookkeeping (no game; built with the service's own sources, run under plain Wine by
// tests/run.sh) against a stand-in KilnApi whose kiln.hooks installs links (they are never called: there is no game):
//   - a send hook kiln.hooks refuses makes stats_enable and watch answer KILN_ERR_UNAVAILABLE (kiln_repl.h), never OK
//     with nothing to come;
//   - a watch's KILN_REPL_KIND_ENDED gives its slot back: 64 watches, the world stops, each gets ENDED, and a new watch
//     finds room without a remove();
//   - capture takes a plain file name in kiln/logs only.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "kiln_api.h"
#include "services/kiln_caps.h"
#include "services/kiln_hooks.h"
#include "services/kiln_net.h"
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

const KilnReplApi* g_table = nullptr;
std::string g_fail;  // kiln.hooks refuses links whose label holds this
uint64_t g_links = 0;
KilnWorldEventFn g_world_fn = nullptr;
void* g_world_user = nullptr;
KilnOwnerRetiringFn g_retiring = nullptr;
int g_ended = 0, g_events = 0;

void dummy_game_function() {}
void KILN_CALL api_log(KilnOwner, int32_t, KilnStr) {}
bool KILN_CALL api_feature(KilnStr name) {
    const std::string n(name.ptr, name.len);
    return n == "repl.observe";
}
KilnResult KILN_CALL api_path(int32_t, char* buffer, size_t capacity, size_t* out_len) {
    static const char dir[] = "C:\\repl-watch-test";
    if (out_len) *out_len = sizeof dir - 1;
    if (capacity < sizeof dir - 1) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(buffer, dir, sizeof dir - 1);
    return KILN_OK;
}
KilnResult KILN_CALL api_symbol(KilnStr, uintptr_t* out) {
    *out = reinterpret_cast<uintptr_t>(&dummy_game_function);
    return KILN_OK;
}
KilnResult KILN_CALL api_constant(KilnStr name, int64_t* out) {
    static const std::map<std::string, int64_t> values = {
        {"world.storage", 0xe179c0}, {"repl/world.objects", 0x15fd2a8}, {"repl/obj.stride", 0x125850}, {"repl/obj.client_ctx", 0x8},
        {"repl/obj.pred_ctx", 0x92c18}, {"repl/world.tick", 0xe17e9c}, {"repl/obj.count", 17}, {"net/gs.machines", 0x16a0}};
    auto it = values.find(std::string(name.ptr, name.len));
    if (it == values.end()) return KILN_ERR_NOT_FOUND;
    *out = it->second;
    return KILN_OK;
}
KilnResult KILN_CALL caps_find(KilnStr, KilnCapRow*) { return KILN_ERR_NOT_FOUND; }
KilnResult KILN_CALL caps_report(KilnOwner, KilnStr, uint32_t, uint32_t, KilnStr) { return KILN_OK; }
KilnResult KILN_CALL net_status(KilnNetStatus* out) {
    out->lab = 0;
    out->lab_port = 0;
    return KILN_OK;
}
KilnResult KILN_CALL hooks_raw_detour(KilnOwner, uintptr_t target, const KilnHookOrder* order, void*, void** original,
                                      KilnHandle* out) {
    const std::string label = order ? std::string(order->label.ptr, order->label.len) : std::string();
    if (!g_fail.empty() && label.find(g_fail) != std::string::npos) return KILN_ERR_UNAVAILABLE;
    if (original) *original = reinterpret_cast<void*>(target);
    *out = ++g_links;
    return KILN_OK;
}
KilnResult KILN_CALL hooks_revoke(KilnHandle) { return KILN_OK; }
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
    static KilnNetApi net = [] {
        KilnNetApi t{};
        t.header = {sizeof t, KILN_NET_VERSION};
        t.status = &net_status;
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
    if (n == KILN_NET_SERVICE) return &net;
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
KilnResult KILN_CALL api_subscribe_world(KilnOwner, KilnWorldEventFn fn, void* user, KilnHandle* out) {
    g_world_fn = fn;
    g_world_user = user;
    *out = 2;
    return KILN_OK;
}
bool KILN_CALL api_on_tick_thread() { return true; }
uintptr_t KILN_CALL api_server_world() { return 0; }
uint32_t KILN_CALL api_side() { return KILN_SIDE_SERVER; }

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
    a.on_tick_thread = &api_on_tick_thread;
    a.server_world = &api_server_world;
    a.side = &api_side;
    return a;
}

void KILN_CALL on_event(const KilnReplWatchEvent* e, void*) {
    ++g_events;
    if (e->kind == KILN_REPL_KIND_ENDED) ++g_ended;
}

void stop_world() {
    KilnWorldEvent ev{};
    ev.size = sizeof ev;
    ev.event = KILN_WORLD_STOPPING;
    ev.kind = KILN_WORLD_SERVER;
    ev.session = 1;
    if (g_world_fn) g_world_fn(&ev, g_world_user);
}

KilnStr str(const char* s) { return KilnStr{s, std::strlen(s)}; }

}  // namespace

int main() {
    static const KilnApi api = make_api();
    KilnPluginInfo info{};
    info.size = sizeof info;
    g_case = "start";
    CHECK(kiln_plugin_main(&api, 7, &info) == KILN_OK && g_table);
    if (!g_table) return 1;
    CHECK((g_table->capabilities() & KILN_REPL_CAP_OBSERVE) != 0);  // the send step went in
    CHECK(g_retiring != nullptr);

    // kiln.hooks refuses the send hooks: nothing may answer OK and then never deliver
    g_case = "send hooks refused";
    g_fail = "repl/stats";
    CHECK(g_table->stats_enable(9, true) == KILN_ERR_UNAVAILABLE);
    uint32_t rows = 99;
    CHECK(g_table->peer_stats(nullptr, 0, &rows) == KILN_OK && rows == 0);
    KilnHandle h = 99;
    CHECK(g_table->watch(9, 100, KILN_REPL_ALL_MACHINES, KILN_REPL_SEND, &on_event, nullptr, &h) == KILN_ERR_UNAVAILABLE && h == 0);
    g_fail.clear();
    CHECK(g_table->stats_enable(9, true) == KILN_OK);
    CHECK(g_table->stats_enable(9, false) == KILN_OK);

    // ENDED gives the slot back: 64 watches, the world stops, a 65th fits without a remove()
    g_case = "ended frees the slot";
    KilnHandle hs[64] = {};
    bool all = true;
    for (int i = 0; i < 64; ++i)
        all = all && g_table->watch(9, 1000 + i, KILN_REPL_ALL_MACHINES, KILN_REPL_SEND, &on_event, nullptr, &hs[i]) == KILN_OK;
    CHECK(all);
    CHECK(g_table->watch(9, 2000, KILN_REPL_ALL_MACHINES, KILN_REPL_SEND, &on_event, nullptr, &h) == KILN_ERR_INVALID_ARGUMENT);
    stop_world();
    CHECK(g_ended == 64 && g_events == 64);
    CHECK(g_table->watch(9, 2000, KILN_REPL_ALL_MACHINES, KILN_REPL_SEND, &on_event, nullptr, &h) == KILN_OK && h != 0);
    CHECK(g_table->remove(hs[0]) == KILN_ERR_NOT_FOUND);  // already given back
    CHECK(g_table->remove(h) == KILN_OK);
    CHECK(g_table->remove(0) == KILN_ERR_INVALID_ARGUMENT);

    // a revoked owner's watches go (its fn is never called again)
    g_case = "owner revoked";
    CHECK(g_table->watch(11, 3000, KILN_REPL_ALL_MACHINES, KILN_REPL_SEND, &on_event, nullptr, &h) == KILN_OK);
    if (g_retiring) g_retiring(11, nullptr);
    const int before = g_events;
    stop_world();  // would deliver ENDED to a live watch
    CHECK(g_events == before);

    // capture: a plain file name in kiln/logs, nothing else (checked before the peer)
    g_case = "capture path";
    for (const char* bad : {"..\\x.jsonl", "C:\\x.jsonl", "\\\\server\\share\\x.jsonl", "a/b.jsonl", "a\\b.jsonl", "x:y"})
        CHECK(g_table->capture(9, 0x81, 5, str(bad)) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(g_table->capture(9, 0x81, 5, str("repl-ok.jsonl")) == KILN_ERR_INVALID_ARGUMENT);  // the peer is not connected

    std::printf("repl watch tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
