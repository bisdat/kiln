// SPDX-License-Identifier: MIT
// Dedicated-server check of the native station calls: an 88-byte fill (no chests), a full-size fill that names an
// ordinary chest, and collect with KILN_STATION_COLLECT_TO_BASE. Logs station-native-test: PASS/FAIL. The C# lab
// mods/tests/station-chests-test places the props; this mod uses a station and a chest already in the world.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "kiln_native.h"
#include "services/kiln_stations.h"
#include "services/kiln_world.h"

namespace {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
const KilnStationsApi* g_st = nullptr;
int g_phase = 0;
int g_waiting = 0;
int g_pass = 0;
int g_fail = 0;
KilnEntity g_station = 0;
KilnEntity g_chest = 0;
int32_t g_base = -1;

void say(const char* text) {
    size_t n = 0;
    while (text[n]) ++n;
    g_api->log(g_owner, KILN_LOG_INFO, KilnStr{text, n});
}

void line(bool ok, const char* what) {
    char buf[400];
    std::snprintf(buf, sizeof buf, "station-native-test: %s %s", ok ? "PASS" : "FAIL", what);
    if (ok) ++g_pass;
    else ++g_fail;
    say(buf);
}

bool slot_ready(size_t end) { return g_st && g_st->header.size >= end; }

void KILN_CALL on_done(const KilnStationResult* r, void* user) {
    const int which = static_cast<int>(reinterpret_cast<intptr_t>(user));
    char buf[360];
    const int32_t status = r ? r->status : KILN_STATION_UNAVAILABLE;
    const char* msg = r ? r->message : "";
    bool ok = false;
    const char* name = "call";
    if (which == 1) {
        name = "88-byte fill";
        ok = status != KILN_STATION_UNAVAILABLE && status != KILN_STATION_DROPPED && r && r->from_chests == 0;
    } else if (which == 2) {
        name = "full-size fill naming a chest";
        ok = status == KILN_STATION_DONE || status == KILN_STATION_PARTIAL || status == KILN_STATION_NOT_ENOUGH ||
             status == KILN_STATION_NO_ROOM || status == KILN_STATION_NO_RECIPE;
    } else {
        name = "collect to base";
        ok = status == KILN_STATION_DONE || status == KILN_STATION_PARTIAL || status == KILN_STATION_NO_ROOM ||
             status == KILN_STATION_UNCHANGED;
    }
    std::snprintf(buf, sizeof buf, "%s status %d (%s)", name, status, msg);
    line(ok, buf);
    g_waiting = 0;
    g_phase = which + 1;
}

bool dedicated() {
    KilnSessionInfo s{};
    s.size = sizeof s;
    if (!g_api->session || g_api->session(&s) != KILN_OK) return false;
    return s.state == KILN_SESSION_DEDICATED;
}

bool pick_station() {
    KilnStationsStatus st{};
    st.size = sizeof st;
    if (g_st->status(&st) != KILN_OK || !st.read || !st.scanned) return false;
    KilnEntity ids[64];
    uint32_t n = 0;
    if (g_st->stations(-1, ids, 64, &n) != KILN_OK && n == 0) return false;
    if (n == 0) return false;
    g_station = ids[0];
    KilnStation info{};
    info.size = sizeof info;
    if (g_st->station_read(g_station, &info) != KILN_OK) return false;
    g_base = info.base_index;
    return true;
}

KilnEntity ordinary_chest() {
    if (g_base < 0) return 0;
    KilnEntity stock[64];
    uint32_t sn = 0;
    g_st->base_storage(static_cast<uint32_t>(g_base), stock, 64, &sn);
    const KilnWorldApi* w = static_cast<const KilnWorldApi*>(g_api->service(KilnStr{KILN_WORLD_SERVICE, sizeof KILN_WORLD_SERVICE - 1}, 1));
    if (!w || w->header.size < offsetof(KilnWorldApi, find) + sizeof(void*) || !w->find || !w->component_hash) return 0;
    const uint32_t hash = w->component_hash(KilnStr{"Inventory", 9});
    KilnEntity ids[128];
    uint32_t n = 0;
    if (w->find(hash, ids, 128, &n) != KILN_OK && n == 0) return 0;
    for (uint32_t i = 0; i < n && i < 128; ++i) {
        const KilnEntity e = ids[i];
        if (e == 0 || e <= 1023 || e == g_station) continue;
        KilnStation station{};
        station.size = sizeof station;
        if (g_st->station_read(e, &station) == KILN_OK) continue;
        bool magic = false;
        for (uint32_t k = 0; k < sn && k < 64; ++k)
            if (stock[k] == e) magic = true;
        if (!magic) return e;
    }
    return 0;
}

bool submit_fill(uint32_t size, const KilnEntity* chest, int which) {
    KilnStationRequest r{};
    r.size = size;
    r.action = KILN_STATION_FILL;
    r.flags = KILN_STATION_DRY_RUN | KILN_STATION_FILL_ADD;
    r.station = g_station;
    r.crafts = 1;
    if (size >= sizeof r && chest) {
        r.chest_count = 1;
        r.chests = chest;
    }
    uint64_t ticket = 0;
    const KilnResult rc = g_st->submit(g_owner, &r, &on_done, reinterpret_cast<void*>(static_cast<intptr_t>(which)), &ticket);
    if (rc != KILN_OK) {
        char buf[80];
        std::snprintf(buf, sizeof buf, "fill size %u was not accepted (%d)", size, rc);
        line(false, buf);
        return false;
    }
    g_waiting = 1;
    return true;
}

void KILN_CALL on_pre(const KilnTickInfo*, void*) {
    if (g_phase < 0 || g_waiting) return;
    if (!g_st) {
        g_st = static_cast<const KilnStationsApi*>(g_api->service(KilnStr{KILN_STATIONS_SERVICE, sizeof KILN_STATIONS_SERVICE - 1}, 1));
        if (!g_st) return;
    }
    if (!dedicated()) return;
    if (g_phase == 0) {
        if (!pick_station()) return;
        g_phase = 1;
    }
    if (g_phase == 1) {
        if (!g_st->submit) {
            line(false, "no submit");
            g_phase = -1;
            return;
        }
        if (!submit_fill(KILN_STATION_REQUEST_V1, nullptr, 1)) g_phase = 2;
        return;
    }
    if (g_phase == 2) {
        g_chest = ordinary_chest();
        if (!g_chest) {
            line(false, "full-size fill: no ordinary chest in the world");
            g_phase = 3;
            return;
        }
        if (!submit_fill(sizeof(KilnStationRequest), &g_chest, 2)) g_phase = 3;
        return;
    }
    if (g_phase == 3) {
        if (!slot_ready(offsetof(KilnStationsApi, collect) + sizeof(void*)) || !g_st->collect) {
            line(false, "collect is not on this table");
            g_phase = 4;
            return;
        }
        KilnStationCollect c{};
        c.size = sizeof c;
        c.station = g_station;
        c.flags = KILN_STATION_COLLECT_OUTPUTS | KILN_STATION_COLLECT_TO_BASE;
        uint64_t ticket = 0;
        const KilnResult rc = g_st->collect(g_owner, &c, &on_done, reinterpret_cast<void*>(static_cast<intptr_t>(3)), &ticket);
        if (rc != KILN_OK) {
            char buf[80];
            std::snprintf(buf, sizeof buf, "collect was not accepted (%d)", rc);
            line(false, buf);
            g_phase = 4;
            return;
        }
        g_waiting = 1;
        return;
    }
    if (g_phase == 4) {
        if (slot_ready(offsetof(KilnStationsApi, base_stock_with_stations) + sizeof(void*)) && g_st->base_stock_with_stations &&
            g_base >= 0) {
            KilnStationStack rows[8];
            uint32_t n = 0;
            const KilnResult rc = g_st->base_stock_with_stations(static_cast<uint32_t>(g_base), rows, 8, &n);
            char buf[80];
            std::snprintf(buf, sizeof buf, "stock with stations rc %d count %u", rc, n);
            line(rc == KILN_OK || rc == KILN_ERR_BUFFER_TOO_SMALL, buf);
        }
        char buf[80];
        std::snprintf(buf, sizeof buf, "station-native-test done: %d PASS, %d FAIL", g_pass, g_fail);
        say(buf);
        g_phase = -1;
    }
}
}  // namespace

extern "C" __declspec(dllexport) int KILN_CALL kiln_native_init(const KilnNativeContext* ctx) {
    if (!ctx || ctx->size < sizeof(KilnNativeContext) || !ctx->api) return KILN_NATIVE_INIT_UNSUPPORTED;
    g_api = ctx->api;
    g_owner = ctx->owner;
    say("station-native-test: loaded");
    KilnHandle h = 0;
    if (ctx->hooks_enabled && g_api->subscribe_tick(g_owner, KILN_TICK_PRE, &on_pre, nullptr, &h) == KILN_OK)
        say("station-native-test: waiting for a dedicated server");
    else
        say("station-native-test: FAIL no server tick");
    return KILN_NATIVE_INIT_OK;
}
