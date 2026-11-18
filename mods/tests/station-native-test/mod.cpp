// SPDX-License-Identifier: MIT
// Dedicated-server check of the native kiln.stations calls, on props that mods/tests/station-chests-test prepares:
//  - kiln.items' container search (KILN_ITEMS_FIND_CONTAINERS) finds the ordinary chest in the station's base;
//  - an 88-byte FILL (KILN_STATION_REQUEST_V1, no chests) is NOT_ENOUGH when only that chest holds the input;
//  - a FILL naming a Magic chest is REFUSED;
//  - a full-size FILL naming the ordinary chest is DONE, from_chests = one craft's worth, the chest gave it;
//  - base_stock_with_stations = base_stock + the kiln's finished stacks;
//  - collect with the old 0x4 flag is refused; collect TO_BASE is DONE, never reads as a dry run, empties the kiln.
// The C# mod calls station_native_test_stage() once the props are ready and polls station_native_test_state().
// Logs station-native-test: PASS/FAIL lines and "station-native-test done: N PASS, M FAIL".
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "kiln_native.h"
#include "services/kiln_items.h"
#include "services/kiln_stations.h"

namespace {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
const KilnStationsApi* g_st = nullptr;
const KilnItemsApi* g_it = nullptr;

// staged by the C# half (tick thread), read in Pre (tick thread)
std::atomic<int> g_phase{0};  // 0 idle, 1.. running, 100 done
KilnEntity g_station = 0, g_kiln = 0, g_expect = 0, g_chest = 0, g_magic = 0;
uint32_t g_item = 0, g_per = 0, g_base = 0;
uint32_t g_chest_before = 0;
long long g_with0 = -1, g_plain0 = -1, g_kiln0 = -1;  // base_stock_with_stations / base_stock of the item, the kiln's
bool g_waiting = false;
std::atomic<int> g_pass{0}, g_fail{0};

void say(const char* text) { g_api->log(g_owner, KILN_LOG_INFO, KilnStr{text, std::strlen(text)}); }

void line(bool ok, const char* fmt, long long a = 0, long long b = 0, long long c = 0, long long d = 0) {
    char what[300];
    std::snprintf(what, sizeof what, fmt, a, b, c, d);
    char buf[360];
    std::snprintf(buf, sizeof buf, "station-native-test: %s %s", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail).fetch_add(1);
    say(buf);
}

bool has_slot(size_t end) { return g_st && g_st->header.size >= end; }

// What `e` holds of g_item (kiln.items read). -1: unreadable.
long long holds(KilnEntity e) {
    static KilnItemsSlot slots[8 * KILN_ITEMS_MAX_INVENTORIES];
    KilnItemsInventory info{};
    info.size = sizeof info;
    uint32_t n = 0;
    const KilnResult rc = g_it->read(e, &info, slots, 8 * KILN_ITEMS_MAX_INVENTORIES, &n);
    if (rc != KILN_OK && rc != KILN_ERR_BUFFER_TOO_SMALL) return -1;
    long long sum = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (slots[i].item == g_item) sum += slots[i].count;
    return sum;
}

// base_stock_with_stations and base_stock of g_item in g_base. The first's result code.
KilnResult stocks(long long* with_n, long long* plain_n) {
    static KilnStationStack with[1024], plain[1024];
    uint32_t wn = 0, pn = 0;
    const KilnResult a = g_st->base_stock_with_stations(g_base, with, 1024, &wn);
    const KilnResult b = g_st->base_stock(g_base, plain, 1024, &pn);
    *with_n = *plain_n = 0;
    for (uint32_t i = 0; i < wn && i < 1024; ++i)
        if (with[i].item == g_item) *with_n = with[i].count;
    for (uint32_t i = 0; i < pn && i < 1024; ++i)
        if (plain[i].item == g_item) *plain_n = plain[i].count;
    return a != KILN_OK ? a : b;
}

bool in_list(const KilnEntity* ids, uint32_t n, KilnEntity e) {
    for (uint32_t i = 0; i < n; ++i)
        if (ids[i] == e) return true;
    return false;
}

// kiln.items' container search, matched to the station's base: not a Magic chest, not a station, holding the item.
void pick_chest() {
    KilnStationBase b{};
    b.size = sizeof b;
    if (g_st->base_of(g_station, &b) != KILN_OK) {
        line(false, "base_of(station %lld) failed", g_station);
        return;
    }
    g_base = b.index;
    static KilnEntity magic[256];
    uint32_t mn = 0;
    g_st->base_storage(g_base, magic, 256, &mn);
    g_magic = mn ? magic[0] : 0;
    static KilnEntity ids[8192];
    uint32_t n = 0;
    const KilnResult rc = g_it->find_inventories(KILN_ITEMS_FIND_CONTAINERS | KILN_ITEMS_FIND_NONEMPTY, ids, 8192, &n);
    if (rc != KILN_OK && rc != KILN_ERR_BUFFER_TOO_SMALL) {
        line(false, "find_inventories failed (%lld)", rc);
        return;
    }
    if (n > 8192) n = 8192;
    uint32_t candidates = 0;
    bool expected = false;
    for (uint32_t i = 0; i < n; ++i) {
        const KilnEntity e = ids[i];
        if (e == g_station || e == g_kiln || in_list(magic, mn < 256 ? mn : 256, e)) continue;
        KilnStation s{};
        s.size = sizeof s;
        if (g_st->station_read(e, &s) == KILN_OK) continue;  // a production station
        KilnStationBase eb{};
        eb.size = sizeof eb;
        if (g_st->base_of(e, &eb) != KILN_OK || eb.index != g_base) continue;
        if (holds(e) <= 0) continue;
        ++candidates;
        if (e == g_expect) expected = true;
        if (!g_chest || e == g_expect) g_chest = e;
    }
    line(expected && g_magic != 0, "container search: %lld candidate(s) in base %lld hold the item, the prepared chest %lld among them; Magic chest %lld",
         candidates, g_base, g_expect, g_magic);
    g_chest_before = static_cast<uint32_t>(holds(g_chest));
}

void KILN_CALL on_done(const KilnStationResult* r, void* user) {
    const int which = static_cast<int>(reinterpret_cast<intptr_t>(user));
    const int32_t st = r ? r->status : -1;
    const bool tail = r && r->size >= offsetof(KilnStationResult, reserved_tail) + sizeof r->reserved_tail;
    const long long from_chests = tail ? r->from_chests : -1;
    const char* msg = r ? r->message : "";
    char m[200];
    std::snprintf(m, sizeof m, "%s", msg);
    if (which == 2) {
        line(st == KILN_STATION_NOT_ENOUGH && from_chests == 0, "88-byte fill without chests: status %lld (want NOT_ENOUGH %lld), from_chests %lld",
             st, KILN_STATION_NOT_ENOUGH, from_chests);
    } else if (which == 3) {
        line(st == KILN_STATION_REFUSED, "a fill naming the Magic chest: status %lld (want REFUSED %lld)", st, KILN_STATION_REFUSED);
    } else if (which == 4) {
        line(st == KILN_STATION_DONE && tail && from_chests == static_cast<long long>(g_per),
             "a fill naming the ordinary chest: status %lld (want DONE), from_chests %lld (want %lld), result size %lld", st, from_chests, g_per,
             r ? r->size : 0);
    } else if (which == 6) {
        const bool dry = r && (r->flags & KILN_STATION_DRY_RUN) != 0;
        const bool to_base = r && (r->flags & KILN_STATION_COLLECT_TO_BASE) != 0;
        line(st == KILN_STATION_DONE && !dry && to_base && r->action == KILN_STATION_COLLECT && r->change_count > 0,
             "collect TO_BASE: status %lld (want DONE), flags 0x%llx (no DRY_RUN bit), %lld ledger rows", st, r ? r->flags : 0,
             r ? r->change_count : 0);
    }
    if (st != KILN_STATION_DONE && m[0]) {
        char buf[260];
        std::snprintf(buf, sizeof buf, "station-native-test: INFO step %d message: %s", which, m);
        say(buf);
    }
    g_waiting = false;
    g_phase.store(which + 1);
}

bool fill(uint32_t size, const KilnEntity* chest, int which) {
    KilnStationRequest r{};
    r.size = size;
    r.action = KILN_STATION_FILL;
    r.flags = KILN_STATION_FILL_ADD | KILN_STATION_FILL_EXACT | KILN_STATION_FILL_NO_OUTPUTS;
    r.station = g_station;
    r.crafts = 1;
    if (size >= sizeof r && chest) {
        r.chest_count = 1;
        r.chests = chest;
    }
    uint64_t ticket = 0;
    const KilnResult rc = g_st->submit(g_owner, &r, &on_done, reinterpret_cast<void*>(static_cast<intptr_t>(which)), &ticket);
    if (rc != KILN_OK) {
        line(false, "a fill of size %lld was not accepted (%lld)", size, rc);
        return false;
    }
    g_waiting = true;
    return true;
}

void finish() {
    char buf[96];
    std::snprintf(buf, sizeof buf, "station-native-test done: %d PASS, %d FAIL", g_pass.load(), g_fail.load());
    say(buf);
    g_phase.store(100);
}

void KILN_CALL on_pre(const KilnTickInfo*, void*) {
    const int phase = g_phase.load();
    if (phase <= 0 || phase >= 100 || g_waiting) return;
    switch (phase) {
    case 1:
        pick_chest();
        g_phase.store(g_chest ? 2 : 100);
        if (!g_chest) finish();
        return;
    case 2:
        if (!fill(KILN_STATION_REQUEST_V1, nullptr, 2)) g_phase.store(3);
        return;
    case 3:
        if (!g_magic || !fill(sizeof(KilnStationRequest), &g_magic, 3)) g_phase.store(4);
        return;
    case 4:
        g_chest_before = static_cast<uint32_t>(holds(g_chest));
        if (!fill(sizeof(KilnStationRequest), &g_chest, 4)) g_phase.store(5);
        return;
    case 5: {
        const long long now = holds(g_chest);
        line(now == static_cast<long long>(g_chest_before) - g_per, "the ordinary chest gave one craft: %lld -> %lld", g_chest_before, now);
        // The base's other stations (the save's) may hold finished items too: check the kiln's share by moving it.
        // Now: with >= stock + the kiln's; after the collect below, with stays and stock grows by the kiln's.
        if (!has_slot(offsetof(KilnStationsApi, base_stock_with_stations) + sizeof(void*)) || !g_st->base_stock_with_stations) {
            line(false, "base_stock_with_stations is not on this table");
        } else {
            const KilnResult a = stocks(&g_with0, &g_plain0);
            g_kiln0 = holds(g_kiln);
            line(a == KILN_OK && g_kiln0 > 0 && g_with0 >= g_plain0 + g_kiln0,
                 "base_stock_with_stations %lld >= base_stock %lld + the kiln's finished %lld (rc %lld)", g_with0, g_plain0, g_kiln0, a);
        }
        if (!has_slot(offsetof(KilnStationsApi, collect) + sizeof(void*)) || !g_st->collect) {
            line(false, "collect is not on this table");
            finish();
            return;
        }
        KilnStationCollect old{};
        old.size = sizeof old;
        old.station = g_kiln;
        old.flags = KILN_STATION_COLLECT_OUTPUTS | 0x4u;  // the bit TO_BASE had before it moved: no collect flag
        uint64_t t = 0;
        line(g_st->collect(g_owner, &old, nullptr, nullptr, &t) == KILN_ERR_INVALID_ARGUMENT, "collect with flag 0x4 is refused");
        KilnStationCollect c{};
        c.size = sizeof c;
        c.station = g_kiln;
        c.item = g_item;
        c.flags = KILN_STATION_COLLECT_OUTPUTS | KILN_STATION_COLLECT_TO_BASE;
        const KilnResult rc = g_st->collect(g_owner, &c, &on_done, reinterpret_cast<void*>(static_cast<intptr_t>(6)), &t);
        if (rc != KILN_OK) {
            line(false, "collect was not accepted (%lld)", rc);
            finish();
            return;
        }
        g_waiting = true;
        return;
    }
    case 7: {
        line(holds(g_kiln) == 0, "the kiln holds no more finished charcoal: %lld", holds(g_kiln));
        long long w = -1, p = -1;
        if (g_with0 >= 0 && stocks(&w, &p) == KILN_OK)
            line(w == g_with0 && p == g_plain0 + g_kiln0,
                 "after the collect the kiln's share moved into base_stock: with stations %lld (was %lld), base_stock %lld (want %lld)", w,
                 g_with0, p, g_plain0 + g_kiln0);
        finish();
        return;
    }
    default:
        return;
    }
}
}  // namespace

// Called by station-chests-test on the tick thread once the props are ready: the target station (stopped, its recipe
// selected, the other inputs in a Magic chest), the kiln (finished `item` in it, not opted in), the item, one craft's
// count of it, and the ordinary chest it prepared (the search must find it). 1: started; 0: not loaded or busy.
extern "C" __declspec(dllexport) int KILN_CALL station_native_test_stage(uint32_t station, uint32_t kiln, uint32_t item, uint32_t per_craft,
                                                                         uint32_t chest) {
    if (!g_api || g_phase.load() != 0) return 0;
    g_st = static_cast<const KilnStationsApi*>(g_api->service(KilnStr{KILN_STATIONS_SERVICE, sizeof KILN_STATIONS_SERVICE - 1}, 1));
    g_it = static_cast<const KilnItemsApi*>(g_api->service(KilnStr{KILN_ITEMS_SERVICE, sizeof KILN_ITEMS_SERVICE - 1}, 1));
    if (!g_st || !g_it || !has_slot(offsetof(KilnStationsApi, max_chests) + sizeof(void*))) {
        line(false, "kiln.stations with ordinary chests, or kiln.items, is not here");
        finish();
        return 1;
    }
    g_station = station;
    g_kiln = kiln;
    g_item = item;
    g_per = per_craft;
    g_expect = chest;
    g_phase.store(1);
    return 1;
}

// 0 idle, 1..99 running, 100 done; pass and fail counts.
extern "C" __declspec(dllexport) int KILN_CALL station_native_test_state(int* pass, int* fail) {
    if (pass) *pass = g_pass.load();
    if (fail) *fail = g_fail.load();
    return g_phase.load();
}

extern "C" __declspec(dllexport) int KILN_CALL kiln_native_init(const KilnNativeContext* ctx) {
    if (!ctx || ctx->size < sizeof(KilnNativeContext) || !ctx->api) return KILN_NATIVE_INIT_UNSUPPORTED;
    g_api = ctx->api;
    g_owner = ctx->owner;
    KilnHandle h = 0;
    if (!ctx->hooks_enabled || g_api->subscribe_tick(g_owner, KILN_TICK_PRE, &on_pre, nullptr, &h) != KILN_OK) {
        say("station-native-test: FAIL no server tick");
        return KILN_NATIVE_INIT_FAILED;
    }
    say("station-native-test: loaded, waiting for station-chests-test to stage the props");
    return KILN_NATIVE_INIT_OK;
}
