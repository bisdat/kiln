// SPDX-License-Identifier: MIT
// kiln.stations.client (since 0.2.0-dev.31.13, mod API 0.2.22): a read-only copy of the game client's own station cache
// (ClientFactoryInfo), for a player who joined someone else's world. ABI: native/core/abi/services/kiln_stations_client.h;
// pure part: client_stations_logic.h (host test tests/client_stations_test.cpp); symbols: sigdb client.d/
// stations-client-view.json (tests/client_stations_sigdb_test.py); evidence: docs/re/stations.md section 14.
//
// The copy is taken on the client world's thread after its step (Post: its jobs are done), only while a caller read in
// the last KILN_STATIONS_CLIENT_DEMAND_MS; readers on any thread get that copy. Nothing here writes game memory: the game's
// FactorySpeed and RecipeFind (read-only functions) are the only game code it calls, under SEH.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "client_stations_logic.h"
#include "engine.h"
#include "services/kiln_caps.h"
#include "services/kiln_items.h"
#include "services/kiln_stations_client.h"
#include "stations.h"
#include "xfer.h"

namespace kitems {
namespace {

using namespace client_view;
using CvStack = client_view::Stack;  // kitems::Stack (engine.h) is another type
#define CVLOGI(...) items_log(KILN_LOG_INFO, __VA_ARGS__)
#define CVLOGW(...) items_log(KILN_LOG_WARN, __VA_ARGS__)

constexpr const char* kFeature = "stations.client_view";
constexpr uint32_t kInventoryEvery = 15;  // copies between reads of the stations' inventories (a quarter second at 60 Hz)

using SpeedFn = float(__fastcall*)(void* balancing);

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
std::atomic<bool> g_on{false}, g_fault{false}, g_world{false};
std::atomic<uint64_t> g_read_ms{0};
RowLayout g_l;
int64_t g_count_at = 8, g_clock_at = 0, g_duration_at = 0x38;
SpeedFn g_speed = nullptr;

std::mutex g_mu;  // the copy and the detail
std::vector<KilnStationClientView> g_rows;
uint64_t g_step = 0, g_copy_ms = 0;
char g_detail[160] = "not resolved yet";
char g_skip[160] = "";  // while on and no copy yet: why the last demanded step took none ("" once one is taken)
// the client world's thread only
uint32_t g_copies = 0;
std::unordered_map<uint32_t, std::vector<CvStack>> g_inventories;
std::vector<uint8_t> g_raw;

KilnStr str(const char* s) { return KilnStr{s, std::strlen(s)}; }

void set_detail(const char* why) {
    std::lock_guard lock(g_mu);
    std::snprintf(g_detail, sizeof g_detail, "%s", why);
}

void skip(const char* why) {
    std::lock_guard lock(g_mu);
    std::snprintf(g_skip, sizeof g_skip, "%s", why);
}

void lower(uint32_t reason, const char* why) {
    set_detail(why);
    const auto* caps = static_cast<const KilnCapsApi*>(g_api ? g_api->service(str(KILN_CAPS_SERVICE), KILN_CAPS_VERSION) : nullptr);
    if (caps && caps->report) caps->report(g_owner, str(kFeature), KILN_CAP_UNAVAILABLE, reason, str(why));
}

bool cst(const char* n, int64_t* out) { return g_api->constant(str(n), out) == KILN_OK; }
bool sym(const char* n, uintptr_t* out) { return g_api->symbol(str(n), out) == KILN_OK && *out; }

uint64_t now_ms() { return GetTickCount64(); }

// ---- guarded reads of the client world (no C++ objects with destructors inside __try) ----
struct RawCopy {
    uintptr_t cfi, play;
    int64_t count_at, clock_at, row_size;
    uint8_t* dst;
    uint64_t cap_rows;
    uint64_t rows;
    int64_t now;
    bool ok;
};
void raw_copy(RawCopy* c) {
    __try {
        const auto* rows = *reinterpret_cast<const uint8_t* const*>(c->cfi);
        const uint64_t n = *reinterpret_cast<const uint64_t*>(c->cfi + static_cast<uintptr_t>(c->count_at));
        c->now = *reinterpret_cast<const int64_t*>(c->play + static_cast<uintptr_t>(c->clock_at));
        if (n > c->cap_rows || (n && !rows)) return;
        if (n) std::memcpy(c->dst, rows, static_cast<size_t>(n * static_cast<uint64_t>(c->row_size)));
        c->rows = n;
        c->ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        c->ok = false;
    }
}
bool speed_of(void* balancing, float* out) {
    __try {
        *out = g_speed(balancing);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void fault(const char* what) {
    if (g_fault.exchange(true)) return;
    char why[160];
    std::snprintf(why, sizeof why, "the client's station cache faulted while %s: off until the game restarts", what);
    CVLOGW("stations: %s", why);
    lower(KILN_CAP_REASON_FAULTED, why);
}

void copy_now(uint64_t step) {
    const uintptr_t w = g_api->world(KILN_WORLD_CLIENT);
    const uintptr_t cfi = w ? g_api->singleton(w, str("ClientFactoryInfo")) : 0;
    const uintptr_t play = w ? g_api->singleton(w, str("G38UiPlayData")) : 0;
    if (!cfi || !play)  // a client world without the cache yet (loading): no copy, the last one stays; status says why
        return skip(!cfi ? "no station cache (ClientFactoryInfo) in the client world yet"
                         : "no G38UiPlayData (the HUD's clock) in the client world yet");
    void* const balancing = reinterpret_cast<void*>(g_api->singleton(w, str("BalancingRegistry")));
    void* const recipes = reinterpret_cast<void*>(g_api->singleton(w, str("g38_items::RecipeRegistry")));
    void* const items = reinterpret_cast<void*>(g_api->singleton(w, str("g38_items::ItemRegistry")));
    g_raw.resize(static_cast<size_t>(KILN_STATIONS_CLIENT_MAX_ROWS * g_l.size));
    RawCopy rc{cfi, play, g_count_at, g_clock_at, g_l.size, g_raw.data(), KILN_STATIONS_CLIENT_MAX_ROWS, 0, 0, false};
    raw_copy(&rc);
    if (!rc.ok) return fault("its rows were read");
    float factor = 1.0f;  // the game's HUD: 1 when the registry is not there
    if (balancing && !speed_of(balancing, &factor)) return fault("the game's FactorySpeed ran");
    const bool inventories = g_copies++ % kInventoryEvery == 0;
    std::unordered_map<uint32_t, RecipeFacts> seen;
    std::vector<KilnStationClientView> out;
    out.reserve(static_cast<size_t>(rc.rows));
    static thread_local std::vector<KilnItemsSlot> slots(8 * KILN_ITEMS_MAX_INVENTORIES);
    std::unordered_map<uint32_t, std::vector<CvStack>> kept;
    for (uint64_t i = 0; i < rc.rows; ++i) {
        const Row row = decode(g_raw.data() + i * static_cast<uint64_t>(g_l.size), g_l);
        if (!row.entity) continue;
        RecipeFacts facts;
        if (row.recipe && recipes) {
            auto it = seen.find(row.recipe);
            if (it == seen.end()) {
                RecipeFacts f;
                f.found = stations_recipe_facts(recipes, row.recipe, g_duration_at, &f.duration_ns, &f.outputs, &f.inputs);
                it = seen.emplace(row.recipe, std::move(f)).first;
            }
            facts = it->second;
        }
        bool have = false;
        if (inventories && items && E.read_ok) {  // kiln.items' reader resolved (items.read)
            ReadOut ro{};
            ro.info.size = sizeof ro.info;
            ro.slots = slots.data();
            ro.capacity = static_cast<uint32_t>(slots.size());
            ro.no_stamp = true;  // the client world's item entities: never into the server world's stamp table
            unsigned long code = 0;
            if (read_inventory(w, items, row.entity, &ro, &code) && ro.found) {
                std::vector<CvStack> st;
                for (uint32_t k = 0; k < ro.written; ++k)
                    if (slots[k].item && slots[k].count) st.push_back({slots[k].item, slots[k].count});
                kept[row.entity] = std::move(st);
            }
        } else if (auto it = g_inventories.find(row.entity); it != g_inventories.end()) {
            kept[row.entity] = it->second;
        }
        auto inv = kept.find(row.entity);
        have = inv != kept.end();
        out.push_back(view_of(row, facts, factor, rc.now, step, have, have ? inv->second : std::vector<CvStack>{}));
    }
    g_inventories = std::move(kept);
    std::lock_guard lock(g_mu);
    g_rows = std::move(out);
    g_step = step;
    g_copy_ms = now_ms();
    g_skip[0] = 0;
}

void KILN_CALL on_post(const KilnTickInfo* tick, void*) {
    g_world = true;
    if (!g_on.load() || g_fault.load()) return;
    if (!demanded(g_read_ms.load(std::memory_order_relaxed), now_ms())) {
        g_copies = 0;  // the first copy after a pause rereads the inventories (the kept ones are from before it)
        return;
    }
    copy_now(tick ? tick->step : 0);
}

void KILN_CALL on_world(const KilnWorldEvent* ev, void*) {
    if (!ev || ev->kind != KILN_WORLD_CLIENT) return;
    if (ev->event != KILN_WORLD_STARTED && ev->event != KILN_WORLD_STOPPING) return;
    // STOPPING needs world.lifecycle; without it the next world's STARTED is the first word that the old one is gone
    g_world = ev->event == KILN_WORLD_STARTED;
    g_inventories.clear();  // both events come on the client world's thread (STOPPING: the destroying thread is it)
    g_copies = 0;
    std::lock_guard lock(g_mu);
    g_rows.clear();  // entity ids of a closed world name nothing
    g_step = g_copy_ms = 0;
}

KilnResult gate() {
    if (!g_api || g_api->side() != KILN_SIDE_CLIENT || !g_on.load() || g_fault.load()) return KILN_ERR_UNAVAILABLE;
    g_read_ms.store(now_ms(), std::memory_order_relaxed);
    return KILN_OK;
}

KilnResult KILN_CALL t_status(KilnStationsClientStatus* out) {
    if (!out || out->size != sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->on = g_on.load() && !g_fault.load();
    out->world = g_api && g_api->world(KILN_WORLD_CLIENT) != 0;
    out->copying = demanded(g_read_ms.load(), now_ms());
    std::lock_guard lock(g_mu);
    out->rows = static_cast<uint32_t>(g_rows.size());
    out->step = g_step;
    out->age_ms = g_copy_ms ? static_cast<int64_t>(now_ms() - g_copy_ms) : -1;
    // off: why; on without a copy: why the last demanded step took none; on with a copy: ""
    std::snprintf(out->detail, sizeof out->detail, "%s", !out->on ? g_detail : g_copy_ms ? "" : g_skip);
    return KILN_OK;
}

KilnResult KILN_CALL t_stations(KilnStationClientView* out, uint32_t capacity, uint32_t* out_count) {
    if (!out_count || (capacity && !out)) return KILN_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    if (KilnResult g = gate(); g != KILN_OK) return g;
    std::lock_guard lock(g_mu);
    return copy_out(g_rows, out, capacity, out_count);
}

KilnResult KILN_CALL t_station(KilnEntity station, KilnStationClientView* out) {
    if (!out || out->size != sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    if (KilnResult g = gate(); g != KILN_OK) return g;
    std::lock_guard lock(g_mu);
    for (const auto& r : g_rows)
        if (r.entity == station) {
            *out = r;
            return KILN_OK;
        }
    return KILN_ERR_NOT_FOUND;
}

const KilnStationsClientApi kTable = {{sizeof(KilnStationsClientApi), KILN_STATIONS_CLIENT_VERSION}, &t_status, &t_stations, &t_station};

}  // namespace

void client_stations_resolve() {
    g_api = items_api();
    g_owner = items_owner();
    if (!g_api) return;
    if (g_api->side() != KILN_SIDE_CLIENT) {
        set_detail("a dedicated server has no client world: kiln.stations has the server's stations");
        return;
    }
    if (!g_api->feature(str(kFeature))) {
        set_detail("stations.client_view did not resolve on this game build");
        return;
    }
    RowLayout l;
    int64_t max_rows = 0, play_clock = -1, cfi_duration = 0, duration = 0;
    uintptr_t speed = 0, hud_speed = 0;
    const bool consts = cst("stations/cfi.row_size", &l.size) && cst("stations/cfi.workshop", &l.workshop) &&
                        cst("stations/cfi.recipe", &l.recipe) && cst("stations/cfi.position", &l.position) &&
                        cst("stations/cfi.start", &l.start) && cst("stations/cfi.sel_category", &l.sel_category) &&
                        cst("stations/cfi.sel_index", &l.sel_index) && cst("stations/cfi.crafts", &l.crafts) &&
                        cst("stations/cfi.water_max", &l.water_max) && cst("stations/cfi.state", &l.state) &&
                        cst("stations/cfi.missing", &l.missing) && cst("stations/cfi.water", &l.water) &&
                        cst("stations/cfi.use_all", &l.use_all) && cst("stations/cfi.energy", &l.energy) &&
                        cst("stations/cfi.base", &l.base) && cst("stations/cfi.count", &g_count_at) &&
                        cst("stations/cfi.max_rows", &max_rows) && cst("stations/play.clock", &play_clock) &&
                        cst("stations/cfi.recipe_duration", &cfi_duration) && cst("stations/recipe.duration", &duration);
    const bool fns = sym("stations/FactorySpeed", &speed) && sym("stations/client.HudFactorySpeed", &hud_speed);
    const char* why = !consts ? "a constant of stations.client_view is missing"
                      : !fns ? "stations/FactorySpeed did not resolve"
                      : hud_speed != speed ? "the HUD's speed call is not stations/FactorySpeed"
                      : !layout_ok(l) ? "the station cache's row layout does not check out"
                      : max_rows != KILN_STATIONS_CLIENT_MAX_ROWS ? "the station cache holds another number of rows"
                      : cfi_duration != duration ? "the HUD's recipe duration is not kiln.stations' recipe duration"
                      : play_clock != 0 ? "the HUD's clock is not where Kiln reads it"
                      : nullptr;
    if (why) {
        CVLOGW("stations: the client's station view is off: %s", why);
        lower(KILN_CAP_REASON_SYMBOL_MISSING, why);
        return;
    }
    g_l = l;
    g_clock_at = play_clock;
    g_duration_at = duration;
    g_speed = reinterpret_cast<SpeedFn>(speed);
    g_on = true;
    set_detail("");
}

void client_stations_register() {
    if (!g_on.load()) return;
    KilnHandle h = 0;
    if (g_api->subscribe_world_tick(g_owner, KILN_WORLD_CLIENT, KILN_TICK_POST, &on_post, nullptr, &h) != KILN_OK ||
        g_api->subscribe_world(g_owner, &on_world, nullptr, &h) != KILN_OK) {
        g_on = false;
        lower(KILN_CAP_REASON_FAULTED, "no tick of the client world for the client's station view");
        return;
    }
    CVLOGI("stations: the client's station view is on (the game's station cache, copied while read)");
}

KilnResult client_stations_publish(const KilnApi* api, KilnOwner owner) {
    return api->register_service(owner, KilnStr{KILN_STATIONS_CLIENT_SERVICE, std::strlen(KILN_STATIONS_CLIENT_SERVICE)}, &kTable);
}

}  // namespace kitems
