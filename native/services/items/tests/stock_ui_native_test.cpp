// SPDX-License-Identifier: MIT
// The ingredient rows' "available" count hook (feature stations.stock_ui) as kiln-items runs it on a Kiln client, on fake
// game functions: which call sites it raises (the production station's row widget, and since GitHub #132 the crafting
// menu's ingredient list and its row's options), the list's fixed-slot term, a fault in it, and the symbols it needs.
// No game, no client: what the game draws is the real-client check.
#include "../stations.cpp"
#include <map>
#include <string>
namespace fixture {
uintptr_t ret = 0;
uint32_t game = 0, fixed = 0, fixed_calls = 0, last_item = 0;
const void* last_view = nullptr;
bool fixed_fault = false;
std::map<std::string, uintptr_t> symbols;
std::map<std::string, uint32_t> lowered;
}
namespace kitems {
Engine E{};
KilnResult KILN_CALL dyes_set_stock(KilnEntity,const KilnStationStack*,uint32_t) { return KILN_ERR_UNAVAILABLE; }
const char* const kAccNames[A_COUNT] = {};
bool verify_access(const uint64_t*,const int*,const int*,const int*,void*,Access*,bool*,char*,unsigned long*) { std::abort(); }
void abandon_tx(uint8_t*) { std::abort(); }
bool note_wanted() { return false; }
NoteLease::~NoteLease() {}
void NoteLease::begin(const Access&,const uint32_t*,uint32_t) { std::abort(); }
void NoteLease::end(const Access&) { std::abort(); }
bool run_move(const Access&,const MoveJob&,uint8_t*,KilnItemsMoveResult*,unsigned long*) { std::abort(); }
void items_count_move(bool) { std::abort(); }
bool read_fuel(uintptr_t,void*,KilnEntity,int32_t*,int32_t*,bool*,unsigned long*) { std::abort(); }
uint64_t items_next_ticket() { std::abort(); }
bool read_inventory(uintptr_t,void*,KilnEntity,ReadOut*,unsigned long*) { std::abort(); }
uintptr_t items_server_world() { return 0; }
void items_log(int,const char*,...) {}
uint64_t current_step() { return 1; }
const KilnApi* items_api() { return g_api; }
KilnOwner items_owner() { return 10; }
bool access_from_tx(void*,Access*) { std::abort(); }
bool setup_flags(const Access&,KilnEntity,uint8_t*) { std::abort(); }
bool read_pages(const Access&,KilnEntity,PageSet*,bool*) { std::abort(); }
void player_guard(const Access&,KilnEntity,PlayerGuard*) { std::abort(); }
bool inventory_matches(uintptr_t,KilnEntity,uint32_t,bool*,unsigned long*) { std::abort(); }
}
namespace {
using namespace kitems;
// stand-ins for the game's UiAvailableCount(item, view, stock map) and its fixed-slot count(item, view)
uint32_t __fastcall count_fn(uint32_t, const void*, const void*) { return fixture::game; }
uint32_t __fastcall fixed_fn(uint32_t item, const void* view) {
    ++fixture::fixed_calls;
    fixture::last_item = item;
    fixture::last_view = view;
    if (fixture::fixed_fault) RaiseException(0xe1230001, 0, 0, nullptr);
    return fixture::fixed;
}
KilnResult KILN_CALL raw(KilnOwner,uintptr_t target,const KilnHookOrder*,void*,void** next,KilnHandle* h) {
    *next = reinterpret_cast<void*>(target);  // the continuation is the stand-in itself: the test calls hk_avail directly
    *h = 1;
    return KILN_OK;
}
KilnResult KILN_CALL revoke(KilnHandle) { return KILN_OK; }
uintptr_t KILN_CALL caller(KilnHandle) { return fixture::ret; }
KilnHooksApi hooks = [] { KilnHooksApi t{}; t.header = {sizeof t, KILN_HOOKS_VERSION}; t.raw_detour = &raw; t.caller = &caller; t.revoke = &revoke; return t; }();
KilnResult KILN_CALL report(KilnOwner,KilnStr feature,uint32_t,uint32_t reason,KilnStr) {
    fixture::lowered[std::string(feature.ptr, feature.len)] = reason;
    return KILN_OK;
}
KilnCapsApi caps = [] { KilnCapsApi t{}; t.header = {sizeof t, KILN_CAPS_VERSION}; t.report = &report; return t; }();
const void* KILN_CALL service(KilnStr name,uint32_t) {
    const std::string s(name.ptr, name.len);
    return s == KILN_HOOKS_SERVICE ? static_cast<const void*>(&hooks) : s == KILN_CAPS_SERVICE ? static_cast<const void*>(&caps) : nullptr;
}
KilnResult KILN_CALL symbol(KilnStr name,uintptr_t* out) {
    const auto it = fixture::symbols.find(std::string(name.ptr, name.len));
    *out = it == fixture::symbols.end() ? 0 : it->second;
    return it == fixture::symbols.end() ? KILN_ERR_NOT_FOUND : KILN_OK;
}
uint32_t KILN_CALL side() { return KILN_SIDE_CLIENT; }
}
int main() {
    using namespace kitems;
    int checks=0,failed=0;
#define CHECK(x) do {++checks;if(!(x)){++failed;std::printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
    KilnApi api{}; api.size=sizeof api; api.service=&service; api.symbol=&symbol; api.side=&side; g_api=&api; g_owner=10;
    constexpr uintptr_t ROW1=0x140bb19b8, ROW2=0x140bb280a, LIST=0x140bf5095, OPTION=0x140bf352c, OTHER=0x140c7eeb4;
    const auto fn = reinterpret_cast<uintptr_t>(&count_fn), fixed = reinterpret_cast<uintptr_t>(&fixed_fn);
    const std::map<std::string, uintptr_t> all = {
        {"stations/client.UiAvailableCount", fn}, {"stations/client.AvailableRowRet1", ROW1},
        {"stations/client.AvailableRowRet2", ROW2}, {"stations/client.AvailableListRet", LIST},
        {"stations/client.AvailableOptionRet", OPTION}, {"stations/client.UiFixedSlotsCount", fixed}};

    // the symbols: each one is required, a missing one lowers stations.stock_ui and nothing is hooked
    for (const auto& [missing, _] : all) {
        fixture::symbols = all; fixture::symbols.erase(missing); fixture::lowered.clear();
        CHECK(!avail_resolve());
        CHECK(fixture::lowered.count("stations.stock_ui") && fixture::lowered["stations.stock_ui"] == KILN_CAP_REASON_SYMBOL_MISSING);
    }
    fixture::symbols = all; fixture::lowered.clear();
    CHECK(avail_resolve() && fixture::lowered.empty());

    // hooked at the first snapshot (a server's: item 7, 20 of it in the bag, the chests and the player's sources)
    const KilnStationStack rows[1] = {{7, 20}};
    CHECK(t_set_craft_stock(rows, 1) == KILN_OK);
    avail_hook_on_demand();
    CHECK(g_avail_hooked.load());
    if (!g_avail_hooked.load()) { std::printf("stock ui: the hook did not go in (%s)\n", h_avail.error() ? h_avail.error() : "?"); return 1; }
    uint8_t view[16] = {}, map[16] = {};
    auto at = [&](uintptr_t ret, uint32_t item, uint32_t game, uint32_t fixed_n) {
        fixture::ret = ret; fixture::game = game; fixture::fixed = fixed_n; fixture::fixed_calls = 0;
        return hk_avail(item, view, map);
    };
    // the crafting menu's list (#132): it adds the 3 in the view's fixed slots after this call, so 17 here shows 20
    CHECK(at(LIST, 7, 5, 3) == 17 && fixture::fixed_calls == 1 && fixture::last_item == 7 && fixture::last_view == view);
    CHECK(at(LIST, 7, 25, 3) == 25 && fixture::fixed_calls == 0);  // the game's own count is more: not even asked
    CHECK(at(LIST, 8, 5, 3) == 5 && fixture::fixed_calls == 0);    // an item the snapshot does not name
    // the row's options (#132) and the production station's two rows: the greater of the two counts
    CHECK(at(OPTION, 7, 5, 3) == 20 && fixture::fixed_calls == 0);
    CHECK(at(ROW1, 7, 5, 3) == 20 && at(ROW2, 7, 0, 0) == 20);
    // any other caller (building costs, a hint...) keeps the game's count
    CHECK(at(OTHER, 7, 5, 3) == 5 && at(0, 7, 5, 3) == 5);
    // a fault in the game's fixed-slot count: the game's own count, counted, and the next call still works
    fixture::fixed_fault = true;
    const uint32_t faults = g_avail_faults.load();
    CHECK(at(LIST, 7, 5, 3) == 5 && g_avail_faults.load() == faults + 1);
    fixture::fixed_fault = false;
    CHECK(at(LIST, 7, 5, 3) == 17);
    // the snapshot cleared (the menu closed): the game's counts everywhere
    CHECK(t_set_craft_stock(nullptr, 0) == KILN_OK);
    CHECK(at(LIST, 7, 5, 3) == 5 && at(OPTION, 7, 5, 3) == 5 && at(ROW1, 7, 5, 3) == 5);
    std::printf("stock ui: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
