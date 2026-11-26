// SPDX-License-Identifier: MIT
// GitHub #132 (0.2.0-dev.31.17, review): Kiln's item fill (KILN_STATION_FILL_ITEM, the station screen's per-ingredient
// "+1" / "+n") on the real kiln-items glue: run_item_fill inside kiln_items_stations' program, t_plan's item plan and
// t_fill_flags, against fakes of the program's component reads, the base's list A, the player's fill-opted links and the
// game's AutoTransferItem (S 0x198bf0, as docs/re/stations.md section 10.9 reads it: the player, then the list in order,
// 4 when still short, then all of it into the station, 2 when it does not fit) on a transaction that commits or rolls
// back. What moves, from where and in which calls; that a short, refused or faulting layer moves nothing at all; that a
// dry run moves nothing; that no link is the station, the player, a Magic chest of the base, another player's, a filtered
// one or a production station. No game: the real press is the real-game check.
#include "../stations.cpp"
#include <cstdarg>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
namespace fixture {
using Inv = std::map<uint32_t, std::map<uint32_t, uint32_t>>;
Inv inventory;                 // committed: what kiln.items' reads and the program's pages show
Inv view;                      // the open transaction's view (a copy at InventoryTxInit, written back at the commit)
Inv untakeable;                // counted by the reads, but the game's take does not get them (equipped, say)
std::set<uint32_t> factories;  // entities with a FactoryStation (never a linked source)
uint32_t room = UINT32_MAX;    // what the station holds of the item at most
struct Call { std::vector<uint32_t> list; uint32_t item, station, amount, player; };
std::vector<Call> calls;
int fault_at = -1;             // the call (0-based) that faults
int commits = 0, rollbacks = 0, invalidated = 0;
bool open = false;
}
namespace kitems {
Engine E{};
KilnResult KILN_CALL dyes_set_stock(KilnEntity,const KilnStationStack*,uint32_t) { return KILN_ERR_UNAVAILABLE; }
const char* const kAccNames[A_COUNT] = {};
bool verify_access(const uint64_t*,const int*,const int*,const int*,void*,Access*,bool*,char*,unsigned long*) { std::abort(); }
void abandon_tx(uint8_t* tx) { E.rollback(tx); E.free_arrays(tx); }
bool note_wanted() { return false; }
NoteLease::~NoteLease() {}
void NoteLease::begin(const Access&,const uint32_t*,uint32_t) { std::abort(); }
void NoteLease::end(const Access&) { std::abort(); }
bool run_move(const Access&,const MoveJob&,uint8_t*,KilnItemsMoveResult*,unsigned long*) { std::abort(); }
void items_count_move(bool) { std::abort(); }
bool read_fuel(uintptr_t,void*,KilnEntity,int32_t*,int32_t*,bool*,unsigned long*) { return false; }
uint64_t items_next_ticket() { static uint64_t t = 0; return ++t; }
bool read_inventory(uintptr_t,void*,KilnEntity e,ReadOut* out,unsigned long*) {
    out->found=fixture::inventory.count(e)!=0;out->written=0;out->info.initialized=out->found;
    for(auto [item,count]:fixture::inventory[e]) if(count && out->written<out->capacity) {
        auto& slot=out->slots[out->written++];slot={};slot.item=item;slot.count=count;
    }
    return true;
}
uintptr_t items_server_world() { return 1; }
std::vector<std::string> logs;
std::mutex logs_mu;
void items_log(int,const char* fmt,...) {
    char b[1024]; va_list a; va_start(a,fmt); std::vsnprintf(b,sizeof b,fmt,a); va_end(a);
    std::lock_guard lock(logs_mu); logs.emplace_back(b);
}
uint64_t current_step() { return 1; }
const KilnApi* items_api() { return g_api; }
KilnOwner items_owner() { return 10; }
bool access_from_tx(void*,Access*) { std::abort(); }
bool access_from_tx_reads(void*,Access*) { std::abort(); }
bool setup_flags(const Access&,KilnEntity e,uint8_t* flags) { *flags = e <= 1023 ? 1 : 0; return fixture::inventory.count(e) != 0; }
bool read_pages(const Access&,KilnEntity e,PageSet* out,bool* initialized) {
    *out = {}; *initialized = fixture::inventory.count(e) != 0;
    if (!*initialized) return false;
    out->n = 1; out->p[0].entity = e; size_t i=0;
    for (auto [item,count] : fixture::inventory[e]) { if(i==8)break; out->p[0].s[i++] = {0,item,count,0}; }
    return true;
}
void player_guard(const Access&,KilnEntity e,PlayerGuard* out) { *out = {}; out->is_player = e <= 1023; }
bool inventory_matches(uintptr_t,KilnEntity e,uint32_t,bool* match,unsigned long*) { *match = e > 1023 && fixture::inventory.count(e); return true; }
}
namespace {
using namespace kitems;
constexpr uint32_t PLAYER = 1, PLAYER2 = 2, STATION = 3300, MAGIC = 2000, L1 = 2100, L2 = 2200, L3 = 2300, L4 = 2400, L5 = 2500;
constexpr uint32_t ITEM = 0x2b0cf1d5, OTHER = 0x77;
// the game's transaction: a view of the inventories that the commit writes back and the rollback drops
void* __fastcall tx_ctor(void* tx) { std::memset(tx, 0, 0x100); return tx; }
void __fastcall tx_init(void*, const void*) { fixture::view = fixture::inventory; fixture::open = true; }
void __fastcall tx_commit(void*) { if (fixture::open) fixture::inventory = fixture::view; fixture::open = false; ++fixture::commits; }
void __fastcall tx_rollback(void*) { fixture::view = fixture::inventory; fixture::open = false; ++fixture::rollbacks; }
void __fastcall tx_free(void*) {}
// AutoTransferItem(tx, {u32* ids, count}*, item, station, u16 amount, player) as the game's code runs it
int32_t __fastcall item_fill(void*, const void* stock, uint32_t item, uint32_t station, uint16_t amount, uint32_t player) {
    const auto* l = static_cast<const uint64_t*>(stock);
    const auto* ids = reinterpret_cast<const uint32_t*>(l[0]);
    fixture::Call c{{}, item, station, amount, player};
    for (uint64_t i = 0; ids && i < l[1]; ++i) c.list.push_back(ids[i]);
    fixture::calls.push_back(c);
    if (static_cast<int>(fixture::calls.size()) - 1 == fixture::fault_at) RaiseException(0xe1230002, 0, 0, nullptr);
    uint32_t rest = amount;
    auto pay = [&](uint32_t e) {
        uint32_t& have = fixture::view[e][item];
        const uint32_t hidden = fixture::untakeable[e][item];
        const uint32_t take = std::min(rest, have > hidden ? have - hidden : 0u);
        have -= take;
        rest -= take;
    };
    if (player) pay(player);
    for (uint32_t e : c.list) if (rest) pay(e);
    if (rest) return 4;
    if (static_cast<uint64_t>(fixture::view[station][item]) + amount > fixture::room) return 2;
    fixture::view[station][item] += amount;
    return 0;
}
void __fastcall invalidate(void*, uint32_t station) { if (station == STATION) ++fixture::invalidated; }
// the program's component reads: the station's FactoryStation and Transform
alignas(8) uint8_t fs[0x68] = {}, transform[24] = {};
PtrSize* __fastcall read_other(PtrSize* out, void*, uint64_t acc, uint32_t e) {
    *out = {};
    if (e == STATION && acc == 1) *out = {fs, sizeof fs};
    if (e == STATION && acc == 2) *out = {transform, sizeof transform};
    return out;
}
// kiln.world's reads for t_plan: the station's FactoryStation and position
const void* KILN_CALL world_component(KilnEntity e, uint32_t hash, uint32_t* size) {
    if (e == STATION && hash == 99) { *size = sizeof fs; return fs; }
    *size = 0; return nullptr;
}
KilnResult KILN_CALL world_position(KilnEntity e, KilnVec3* out) {
    if (e != STATION && e != PLAYER) return KILN_ERR_NOT_FOUND;
    *out = {1000, 50, 1000};
    return KILN_OK;
}
PtrSize* __fastcall component(PtrSize* out, uintptr_t, uint32_t hash, uint32_t e) {
    static uint8_t factory[0x68] = {};
    *out = {};
    if (hash == 99 && fixture::factories.count(e)) *out = {factory, sizeof factory};
    return out;
}
BaseHit* __fastcall base_at(BaseHit* out, void*, const float*) { *out = {}; out->error = 0; out->index = 0; return out; }
bool KILN_CALL tick_thread() { return true; }
void* g_bases_ptr = nullptr;
uintptr_t KILN_CALL singleton(uintptr_t, KilnStr name) {
    return std::string_view(name.ptr, name.len) == "PlayerBasesSystemServer" ? reinterpret_cast<uintptr_t>(g_bases_ptr) : 0;
}
KilnResult KILN_CALL session(KilnSessionInfo* info) { info->state = KILN_SESSION_DEDICATED; return KILN_OK; }
std::map<std::string, uint32_t> lowered;
KilnResult KILN_CALL report(KilnOwner, KilnStr feature, uint32_t, uint32_t reason, KilnStr) { lowered[std::string(feature.ptr, feature.len)] = reason; return KILN_OK; }
KilnCapsApi caps = [] { KilnCapsApi t{}; t.header = {sizeof t, KILN_CAPS_VERSION}; t.report = &report; return t; }();
const void* KILN_CALL service(KilnStr name, uint32_t) {
    return std::string(name.ptr, name.len) == KILN_CAPS_SERVICE ? static_cast<const void*>(&caps) : nullptr;
}
uint32_t KILN_CALL side() { return KILN_SIDE_SERVER; }
}
int main() {
    using namespace kitems;
    using namespace kitems::stations;
    int checks = 0, failed = 0;
#define CHECK(x) do {++checks;if(!(x)){++failed;std::printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
    auto logged = [&](const char* s) { for (const auto& l : logs) if (l.find(s) != std::string::npos) return true; return false; };
    KilnApi api{}; api.size = sizeof api; api.session = &session; api.on_tick_thread = &tick_thread; api.singleton = &singleton;
    api.service = &service; api.side = &side; g_api = &api;
    KilnWorldApi world{}; world.component = &world_component; world.position = &world_position; g_w = &world;
    G.read = G.fill_ok = G.craft_ok = G.registered_ok = G.outputs_ok = true;
    G.fill_items_ok = true; G.fill_item = &item_fill; G.invalidate = &invalidate;
    G.fs_size = sizeof fs; G.fs_stopped = 0x10;
    G.base_at = &base_at; G.bases_count = 0x100; G.stride = 0x80; G.stock_items = 0x108; G.stock_items_count = 0x110;
    G.stock_water = 0x118; G.stock_water_count = 0x120; G.aabb_y = 0x134; G.zone_mask_rel = 0x20;
    E.get_component = &component; E.read_other = &read_other; H_FACTORY = 99;
    E.tx_ctor = &tx_ctor; E.tx_init = &tx_init; E.commit = &tx_commit; E.rollback = &tx_rollback; E.free_arrays = &tx_free;
    E.setup_operator = 0x60;
    alignas(16) static uint8_t tx[0x100]; g_tx = tx;
    g_log_item.budget = 100000;
    // two players, both with CanAccessInventories; the base: one Magic chest (MAGIC) in its list A
    uint8_t infos[0xa0 * 2] = {}; wr(infos, 0, PLAYER); infos[0x9d] = 2; wr(infos, 0xa0, PLAYER2); infos[0xa0 + 0x9d] = 2;
    alignas(8) uint8_t bases[0x400] = {}; wr(bases, 0x10, infos); g_srv_bases.store(bases); g_bases_ptr = bases;
    uint32_t list_a[1] = {MAGIC};
    wr(bases, 0x100, uint32_t{1}); wr(bases, 0x108, static_cast<uint32_t*>(list_a)); wr(bases, 0x110, uint64_t{1});
    for (int k = 0; k < 3; ++k) wr<int64_t>(transform, 8 * k, int64_t{1000} << 32);
    fs[kFsState] = 1;
    // the player's links: L1 Before (10), L2 After (4), MAGIC itself (a link to the base's own chest: counted once), L3 an
    // allow list without the item (50), L4 a link that becomes a production station (30), L5 another player's (40)
    fixture::inventory = {{PLAYER, {{ITEM, 2}}}, {PLAYER2, {}}, {STATION, {{ITEM, 7}}}, {MAGIC, {{ITEM, 3}}}, {L1, {{ITEM, 10}}},
                          {L2, {{ITEM, 4}}}, {L3, {{ITEM, 50}}}, {L4, {{ITEM, 30}}}, {L5, {{ITEM, 40}}}};
    const fixture::Inv start = fixture::inventory;
    auto add = [&](uint32_t player, uint32_t container, uint32_t order, uint32_t filter = 0, std::vector<uint32_t> items = {}) {
        KilnCraftSourceRequest r{sizeof r, player, container, order, filter, static_cast<uint32_t>(items.size()), items.data(),
                                 KILN_CRAFT_SOURCE_FILLS, 0};
        uint64_t h = 0; return t_add_source(10, &r, &h) == KILN_OK ? h : 0;
    };
    CHECK(add(PLAYER, MAGIC, 0) && add(PLAYER, L1, 0) && add(PLAYER, L2, 1) && add(PLAYER, L3, 0, 1, {OTHER}) && add(PLAYER, L4, 0) &&
          add(PLAYER2, L5, 0));
    fixture::factories.insert(L4);  // a production station now: PayMaterial cannot tell its inputs from its outputs
    Run run{}; run.bases = bases; run.acc_factory = 1; run.acc_transform = 2; run.factory_info = &world;
    auto request = [](uint32_t amount, uint32_t flags) {
        KilnStationRequest r{}; r.size = sizeof r; r.action = KILN_STATION_FILL; r.flags = KILN_STATION_FILL_ITEM | flags;
        r.station = STATION; r.crafts = amount; r.item = ITEM; r.player = PLAYER;
        return r;
    };
    auto fill = [&](uint32_t amount, uint32_t flags = KILN_STATION_FILL_LINKED) {
        fixture::calls.clear(); fixture::commits = fixture::rollbacks = fixture::invalidated = 0; logs.clear();
        Req q{}; q.r = request(amount, flags);
        CHECK(request_valid(q.r));
        KilnStationResult res = result_for(q);
        run_item_fill(run, q, &res);
        return res;
    };
    auto held = [](uint32_t e) { return fixture::inventory[e][ITEM]; };
    auto reset = [&] { fixture::inventory = start; fixture::untakeable.clear(); fixture::room = UINT32_MAX; fixture::fault_at = -1; };
    auto unchanged = [&] { return fixture::inventory == start; };
    // no call ever names the station, the player, the base's Magic chest, a filtered link, a production station or another
    // player's link as a linked source
    auto clean_lists = [&] {
        for (const auto& c : fixture::calls)
            if (c.player == 0)
                for (uint32_t e : c.list) if (e != L1 && e != L2) return false;
        return true;
    };

    // ---- t_fill_flags: what the managed side asks before it names FILL_ITEM ----
    CHECK(t_fill_flags() & KILN_STATION_FILL_ITEM);
    CHECK(t_fill_flags() & KILN_STATION_FILL_LINKED);
    G.fill_items_ok = false; CHECK(!(t_fill_flags() & KILN_STATION_FILL_ITEM) && (t_fill_flags() & KILN_STATION_FILL_ADD)); G.fill_items_ok = true;
    g_off.store(true); CHECK(t_fill_flags() == 0); g_off.store(false);

    // ---- t_plan: the item plan counts what run_item_fill will (the bag 2 + the Magic chest 3, links L1 10 + L2 4) ----
    {
        KilnStationRequest r = request(12, KILN_STATION_FILL_LINKED);
        KilnStationPlan p{}; p.size = sizeof p;
        CHECK(t_plan(&r, &p) == KILN_OK && p.status == KILN_STATION_DONE && p.crafts_planned == 12 && p.input_count == 1);
        CHECK(p.inputs[0].item == ITEM && p.inputs[0].count == 1 && p.have[0] == 7 && p.stock[0] == 5 && p.linked[0] == 14);
        r = request(12, 0);  // without the links: short, the game's own press would refuse it too
        p = {}; p.size = sizeof p;
        CHECK(t_plan(&r, &p) == KILN_OK && p.status == KILN_STATION_NOT_ENOUGH && p.crafts_affordable == 5 && p.linked[0] == 0);
        r = request(20, KILN_STATION_FILL_LINKED);  // 19 in all: never a part
        p = {}; p.size = sizeof p;
        CHECK(t_plan(&r, &p) == KILN_OK && p.status == KILN_STATION_NOT_ENOUGH && p.crafts_planned == 0 && p.crafts_affordable == 19);
        G.fill_items_ok = false;
        p = {}; p.size = sizeof p;
        CHECK(t_plan(&r, &p) == KILN_ERR_UNAVAILABLE);
        G.fill_items_ok = true;
    }

    // ---- the bag and the Magic chest cover it: the game's own call alone, as the press would have run it ----
    reset();
    KilnStationResult res = fill(4);
    CHECK(res.status == KILN_STATION_DONE && res.crafts_added == 4 && res.from_linked == 0 && res.crafts_before == 7 && res.crafts_after == 11);
    CHECK(fixture::calls.size() == 1 && fixture::calls[0].player == PLAYER && fixture::calls[0].list == std::vector<uint32_t>{MAGIC} &&
          fixture::calls[0].amount == 4 && fixture::calls[0].item == ITEM && fixture::calls[0].station == STATION);
    CHECK(held(STATION) == 11 && held(PLAYER) == 0 && held(MAGIC) == 1 && held(L1) == 10 && fixture::commits == 1 && fixture::invalidated == 1);
    CHECK(logged("player 1's transfer of 4 x item 0x2b0cf1d5 into station 3300: 4 from the bag and the base's Magic chests, 0 from linked sources"));

    // ---- short by 7: the Before link pays it (player 0, its own list), then the game's call the bag and the chest ----
    reset();
    res = fill(12);
    CHECK(res.status == KILN_STATION_DONE && res.crafts_added == 12 && res.from_linked == 7);
    CHECK(fixture::calls.size() == 2 && fixture::calls[0].player == 0 && fixture::calls[0].list == std::vector<uint32_t>{L1} &&
          fixture::calls[0].amount == 7 && fixture::calls[1].player == PLAYER && fixture::calls[1].list == std::vector<uint32_t>{MAGIC} &&
          fixture::calls[1].amount == 5 && clean_lists());
    CHECK(held(STATION) == 19 && held(PLAYER) == 0 && held(MAGIC) == 0 && held(L1) == 3 && held(L2) == 4 && held(L3) == 50 &&
          held(L4) == 30 && held(L5) == 40);
    CHECK(logged("5 from the bag and the base's Magic chests, 7 from linked sources"));

    // ---- the After link pays last ----
    reset();
    res = fill(18);
    CHECK(res.status == KILN_STATION_DONE && res.from_linked == 13 && fixture::calls.size() == 3 && clean_lists());
    CHECK(fixture::calls[0].list == std::vector<uint32_t>{L1} && fixture::calls[0].amount == 10 &&
          fixture::calls[1].list == std::vector<uint32_t>{L2} && fixture::calls[1].amount == 3 && fixture::calls[2].amount == 5);
    CHECK(held(STATION) == 25 && held(L1) == 0 && held(L2) == 1 && held(MAGIC) == 0 && held(PLAYER) == 0);

    // ---- one more than all of it (2 + 3 + 10 + 4 = 19; the Magic chest's link counts once): nothing asked, nothing moved ----
    reset();
    res = fill(20);
    CHECK(res.status == KILN_STATION_NOT_ENOUGH && res.short_item == ITEM && fixture::calls.empty() && unchanged());
    CHECK(logged("short (the bag, the Magic chests and the fill-opted links hold 19): nothing moved"));

    // ---- a link pays less than it was counted to hold: the game's code 4, everything rolled back ----
    reset();
    fixture::untakeable[L1][ITEM] = 2;
    res = fill(18);
    CHECK(res.status == KILN_STATION_NOT_ENOUGH && res.game_error == 4 && fixture::calls.size() == 1 && fixture::rollbacks == 1 &&
          fixture::commits == 0 && unchanged() && fixture::invalidated == 0);
    CHECK(logged("the game refused it (code 4): nothing moved"));

    // ---- the links paid and the bag then pays less (an equipped stack): the links' share goes back too ----
    reset();
    fixture::untakeable[PLAYER][ITEM] = 2;
    res = fill(12);
    CHECK(res.status == KILN_STATION_NOT_ENOUGH && res.game_error == 4 && fixture::calls.size() == 2 && unchanged());

    // ---- the station has no room for the last layer's share: 2, everything rolled back ----
    reset();
    fixture::room = 20;  // 7 + 10 + 3 fit, the game's 5 do not
    res = fill(18);
    CHECK(res.status == KILN_STATION_NO_ROOM && res.game_error == 2 && fixture::calls.size() == 3 && unchanged());

    // ---- the game faults inside a layer: the transaction is abandoned, nothing moved, the actions off ----
    reset();
    fixture::fault_at = 1;
    lowered.clear();
    res = fill(12);
    CHECK(res.status == KILN_STATION_UNAVAILABLE && fixture::calls.size() == 2 && unchanged() && fixture::commits == 0 &&
          fixture::rollbacks >= 1 && g_off.load() && lowered["stations.fill"] == KILN_CAP_REASON_FAULTED);
    CHECK(std::string(res.message).find("faulted") != std::string::npos);
    g_off.store(false);

    // ---- a dry run: the same calls, then the rollback ----
    reset();
    res = fill(12, KILN_STATION_FILL_LINKED | KILN_STATION_DRY_RUN);
    CHECK(res.status == KILN_STATION_DONE && res.crafts_added == 12 && fixture::calls.size() == 2 && unchanged() && fixture::commits == 0 &&
          fixture::invalidated == 0);

    // ---- without FILL_LINKED the links never pay ----
    reset();
    res = fill(12, 0);
    CHECK(res.status == KILN_STATION_NOT_ENOUGH && fixture::calls.empty() && unchanged());
    res = fill(5, 0);
    CHECK(res.status == KILN_STATION_DONE && fixture::calls.size() == 1 && fixture::calls[0].player == PLAYER && held(L1) == 10);

    // ---- no CanAccessInventories: no link is a source ----
    reset();
    infos[0x9d] = 0;
    res = fill(12);
    CHECK(res.status == KILN_STATION_NOT_ENOUGH && fixture::calls.empty() && unchanged());
    infos[0x9d] = 2;

    // ---- the feature off (an exe where AutoTransferItem did not resolve) ----
    reset();
    G.fill_items_ok = false;
    res = fill(4);
    CHECK(res.status == KILN_STATION_UNAVAILABLE && fixture::calls.empty() && unchanged());
    G.fill_items_ok = true;

    std::printf("item fill: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
