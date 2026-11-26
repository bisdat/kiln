// SPDX-License-Identifier: MIT
// kiln.stations.client (client_stations.cpp, 0.2.0-dev.31.13) against a fake client world, no game: what status says
// while the station cache does not resolve, a new client world without a stop of the old one, the inventory reader of
// kiln.items being off, the inventory refresh after a pause of the demand, and the reads leaving the stamp table alone
// (review 31.13). Runs under Wine: native/services/items/tests/run.sh
#include "../client_stations.cpp"

#include <cstdarg>
#include <string>

namespace fixture {
uintptr_t client_world = 0x5000;
bool cache = true;  // ClientFactoryInfo resolves as a singleton
uint8_t cfi_rows[0xc8 * 2] = {};
struct Cfi {
    const uint8_t* rows;
    uint64_t count;
} cfi{cfi_rows, 1};
int64_t play_clock = 150'000'000'000LL;
uint8_t balancing[8] = {}, recipes[8] = {}, items[8] = {};
int reads = 0;
bool last_no_stamp = false;
KilnTickFn post = nullptr;
KilnWorldEventFn world_ev = nullptr;
}  // namespace fixture

namespace kitems {
Engine E{};
const KilnApi* items_api();
void items_log(int, const char*, ...) {}
KilnOwner items_owner() { return 7; }
bool stations_recipe_facts(void*, uint32_t id, int64_t, int64_t* duration_ns, std::vector<uint32_t>* outputs,
                           std::vector<uint32_t>* inputs) {
    if (id != 0x1234) return false;
    *duration_ns = 10'000'000'000LL;
    *outputs = {500};
    inputs->clear();
    return true;
}
bool read_inventory(uintptr_t, void*, KilnEntity, ReadOut* out, unsigned long*) {
    ++fixture::reads;
    fixture::last_no_stamp = out->no_stamp;
    out->found = true;
    out->slots[0] = KilnItemsSlot{};
    out->slots[0].item = 500;
    out->slots[0].count = 3;
    out->written = 1;
    return true;
}
}  // namespace kitems

namespace {
int checks = 0, failures = 0;
void check(bool b, const char* s) {
    ++checks;
    std::printf("%s %s\n", b ? "PASS" : "FAIL", s);
    failures += !b;
}
std::string S(KilnStr s) { return std::string(s.ptr, s.len); }

float __fastcall speed(void*) { return 1.0f; }

bool KILN_CALL feature(KilnStr) { return true; }
uint32_t KILN_CALL side() { return KILN_SIDE_CLIENT; }
uintptr_t KILN_CALL world(uint32_t kind) { return kind == KILN_WORLD_CLIENT ? fixture::client_world : 0; }
const void* KILN_CALL service(KilnStr, uint32_t) { return nullptr; }
KilnResult KILN_CALL symbol(KilnStr n, uintptr_t* out) {
    const std::string s = S(n);
    *out = (s == "stations/FactorySpeed" || s == "stations/client.HudFactorySpeed") ? reinterpret_cast<uintptr_t>(&speed) : 0;
    return *out ? KILN_OK : KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL constant(KilnStr n, int64_t* out) {
    const std::string s = S(n);
    const kitems::client_view::RowLayout l;
    const std::pair<const char*, int64_t> rows[] = {
        {"stations/cfi.row_size", l.size}, {"stations/cfi.workshop", l.workshop}, {"stations/cfi.recipe", l.recipe},
        {"stations/cfi.position", l.position}, {"stations/cfi.start", l.start}, {"stations/cfi.sel_category", l.sel_category},
        {"stations/cfi.sel_index", l.sel_index}, {"stations/cfi.crafts", l.crafts}, {"stations/cfi.water_max", l.water_max},
        {"stations/cfi.state", l.state}, {"stations/cfi.missing", l.missing}, {"stations/cfi.water", l.water},
        {"stations/cfi.use_all", l.use_all}, {"stations/cfi.energy", l.energy}, {"stations/cfi.base", l.base},
        {"stations/cfi.count", 8}, {"stations/cfi.max_rows", KILN_STATIONS_CLIENT_MAX_ROWS}, {"stations/play.clock", 0},
        {"stations/cfi.recipe_duration", 0x38}, {"stations/recipe.duration", 0x38}};
    for (const auto& r : rows)
        if (s == r.first) return *out = r.second, KILN_OK;
    return KILN_ERR_UNAVAILABLE;
}
uintptr_t KILN_CALL singleton(uintptr_t w, KilnStr n) {
    if (w != fixture::client_world || !w) return 0;
    const std::string s = S(n);
    if (s == "ClientFactoryInfo") return fixture::cache ? reinterpret_cast<uintptr_t>(&fixture::cfi) : 0;
    if (s == "G38UiPlayData") return reinterpret_cast<uintptr_t>(&fixture::play_clock);
    if (s == "BalancingRegistry") return reinterpret_cast<uintptr_t>(fixture::balancing);
    if (s == "g38_items::RecipeRegistry") return reinterpret_cast<uintptr_t>(fixture::recipes);
    if (s == "g38_items::ItemRegistry") return reinterpret_cast<uintptr_t>(fixture::items);
    return 0;
}
KilnResult KILN_CALL sub_tick(KilnOwner, uint32_t, int32_t, KilnTickFn fn, void*, KilnHandle* h) {
    fixture::post = fn;
    *h = 1;
    return KILN_OK;
}
KilnResult KILN_CALL sub_world(KilnOwner, KilnWorldEventFn fn, void*, KilnHandle* h) {
    fixture::world_ev = fn;
    *h = 2;
    return KILN_OK;
}
KilnResult KILN_CALL reg(KilnOwner, KilnStr, const void*) { return KILN_OK; }

KilnApi make_api() {
    KilnApi a{};
    a.size = sizeof a;
    a.feature = &feature;
    a.side = &side;
    a.world = &world;
    a.service = &service;
    a.symbol = &symbol;
    a.constant = &constant;
    a.singleton = &singleton;
    a.subscribe_world_tick = &sub_tick;
    a.subscribe_world = &sub_world;
    a.register_service = &reg;
    return a;
}
KilnApi g_fake = make_api();

void put_row(uint8_t* r, uint32_t entity) {
    std::memset(r, 0, 0xc8);
    std::memcpy(r + 0x00, &entity, 4);
    const uint32_t recipe = 0x1234, crafts = 2;
    std::memcpy(r + 0x08, &recipe, 4);
    const int64_t start = 100'000'000'000LL;
    std::memcpy(r + 0x28, &start, 8);
    std::memcpy(r + 0xa0, &crafts, 4);
    r[0xa6] = KILN_STATION_STATE_RUNNING;
}

uint64_t g_step = 0;
void post() {
    KilnTickInfo t{};
    t.size = sizeof t;
    t.step = ++g_step;
    fixture::post(&t, nullptr);
}
KilnStationsClientStatus status() {
    KilnStationsClientStatus s{};
    s.size = sizeof s;
    kitems::kTable.status(&s);
    return s;
}
// a read: what a caller gets (and the demand it sets)
std::vector<KilnStationClientView> read() {
    std::vector<KilnStationClientView> v(4);
    for (auto& x : v) x.size = sizeof x;
    uint32_t n = 0;
    kitems::kTable.stations(v.data(), static_cast<uint32_t>(v.size()), &n);
    v.resize(std::min<size_t>(n, v.size()));
    return v;
}
void world_event(uint32_t event, uintptr_t w, uint32_t session) {
    KilnWorldEvent ev{};
    ev.size = sizeof ev;
    ev.event = event;
    ev.kind = KILN_WORLD_CLIENT;
    ev.session = session;
    ev.world = w;
    fixture::world_ev(&ev, nullptr);
}
}  // namespace

const KilnApi* kitems::items_api() { return &g_fake; }

int main() {
    put_row(fixture::cfi_rows, 4242);
    kitems::E.read_ok = true;
    kitems::client_stations_resolve();
    kitems::client_stations_register();
    check(kitems::g_on.load() && fixture::post && fixture::world_ev, "the view resolves and subscribes on the fake client");

    // 1: the station cache does not resolve as a singleton: status says why no copy is taken (the row stays up)
    fixture::cache = false;
    read();
    for (int i = 0; i < 5; ++i) post();
    {
        const auto s = status();
        check(s.on == 1 && s.copying == 1 && s.rows == 0 && s.age_ms == -1, "no cache: on, copying, no rows, no copy");
        check(s.detail[0] != 0, "no cache: status.detail says why nothing is copied");
        std::printf("   detail: \"%s\"\n", s.detail);
    }
    fixture::cache = true;
    kitems::g_copies = 0;
    read();
    post();
    {
        const auto s = status();
        check(s.rows == 1 && s.age_ms >= 0 && s.detail[0] == 0, "the cache resolves: a copy, detail empty again");
    }

    // 3: kiln.items' reader is off (items.read did not resolve): no inventory reads, no Outputs
    kitems::E.read_ok = false;
    kitems::g_copies = 0;
    kitems::g_inventories.clear();
    fixture::reads = 0;
    read();
    post();
    {
        const auto v = read();
        check(fixture::reads == 0, "read_ok false: read_inventory is not called");
        check(v.size() == 1 && !(v[0].flags & KILN_STATION_CLIENT_OUTPUTS), "read_ok false: the view has no Outputs");
    }
    kitems::E.read_ok = true;
    kitems::g_copies = 0;
    fixture::reads = 0;
    read();
    post();
    {
        const auto v = read();
        check(fixture::reads == 1 && v.size() == 1 && (v[0].flags & KILN_STATION_CLIENT_OUTPUTS) && v[0].output_count == 1 &&
                  v[0].outputs[0].item == 500 && v[0].outputs[0].count == 3,
              "read_ok true: the station's finished stack is read");
        // 4: the client view's reads never stamp (the stamp table is the server world's)
        check(fixture::last_no_stamp, "the client view reads inventories without stamping");
    }

    // 5: the demand lapses; the first copy after the pause refreshes the inventories
    for (int i = 0; i < 3; ++i) post();  // copies 2..4: no inventory read
    check(fixture::reads == 1, "copies between the 15th read no inventory");
    kitems::g_read_ms = 1;  // the last read long ago: no demand
    post();
    check(kitems::g_copies == 0, "a post without demand resets the copy count");
    fixture::reads = 0;
    read();
    post();
    check(fixture::reads == 1, "the first copy after a pause reads the inventories again");

    // 2: a new client world starts without a stop of the old one (world.lifecycle off): A's rows are gone
    check(read().size() == 1, "world A: one row");
    fixture::cache = false;  // B has no cache yet: no copy of B
    fixture::client_world = 0x6000;
    world_event(KILN_WORLD_STARTED, 0x6000, 2);
    read();
    post();
    {
        const auto v = read();
        const auto s = status();
        check(v.empty() && s.rows == 0 && s.age_ms == -1, "world B started without A stopping: no rows of A");
        KilnStationClientView one{};
        one.size = sizeof one;
        check(kitems::kTable.station(4242, &one) == KILN_ERR_NOT_FOUND, "world B: A's station is not found");
    }

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
