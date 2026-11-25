// SPDX-License-Identifier: MIT
// GitHub #132 (0.2.0-dev.31.12): block placements from the player's building links, on the real kiln-items glue: the
// build cursor's mid hook at build/CursorStockRet (fake registers: eax the base's stock count, ebx the bag's, ebp the blocks
// needed, rsi the cursor query, rdi the cursor context), the server's cache, a Kiln client's snapshot (set_build_stock),
// the RawHook on ApplyMaterials that scopes the payment to the blueprint program, and the hooks' install and stand-down
// on foreign code. No game, no world: what the game draws and sends is the real-game check.
#include "../stations.cpp"
#include <cstdarg>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <set>
#include <thread>
namespace fixture {
std::map<uint32_t,std::map<uint32_t,uint32_t>> inventory;      // what kiln.items' reads and the transaction show
std::map<uint32_t,std::map<uint32_t,uint32_t>> tx_view;        // the transaction's view when it differs
bool server_storage = true, fail_take = false;
uint32_t refuse = 0;
std::vector<std::pair<uint32_t,uint32_t>> taken;               // every PayMaterial take Kiln made (entity, n)
void* expected_tx = nullptr;
bool wrong_tx = false;
uint8_t world_kind = 0;
uint32_t current = 1;                                          // CurrentEntityId's answer
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
bool read_inventory(uintptr_t,void*,KilnEntity e,ReadOut* out,unsigned long*) {
    out->found=fixture::inventory.count(e)!=0;out->written=0;out->info.initialized=out->found;
    for(auto [item,count]:fixture::inventory[e]) if(count && out->written<out->capacity) {
        auto& slot=out->slots[out->written++];slot={};slot.item=item;slot.count=count;
    }
    return true;
}
uintptr_t items_server_world() { return 1; }
std::vector<std::string> logs;
std::mutex logs_mu;  // a hosting client's two worlds log from two threads (the install race below)
void items_log(int,const char* fmt,...) {
    char b[1024]; va_list a; va_start(a,fmt); std::vsnprintf(b,sizeof b,fmt,a); va_end(a);
    std::lock_guard lock(logs_mu); logs.emplace_back(b);
}
uint64_t current_step() { return 1; }
const KilnApi* items_api() { return g_api; }
KilnOwner items_owner() { return 10; }
bool access_from_tx(void*,Access*) { std::abort(); }  // the lock payment never asks for Fuel
bool access_from_tx_reads(void* tx,Access* out) { *out = {}; if (tx != fixture::expected_tx) fixture::wrong_tx = true; return fixture::server_storage; }
bool setup_flags(const Access&,KilnEntity e,uint8_t* flags) { *flags = e <= 1023 ? 1 : 0; return fixture::inventory.count(e) != 0; }
bool read_pages(const Access&,KilnEntity e,PageSet* out,bool* initialized) {
    *out = {}; *initialized = fixture::inventory.count(e) != 0;
    if (!*initialized) return false;
    out->n = 1; out->p[0].entity = e; size_t i=0;
    const auto& view = fixture::tx_view.count(e) ? fixture::tx_view[e] : fixture::inventory[e];
    for (auto [item,count] : view) { if(i==8)break; out->p[0].s[i++] = {0,item,count,0}; }
    return true;
}
void player_guard(const Access&,KilnEntity e,PlayerGuard* out) { *out = {}; out->is_player = e <= 1023; }
bool inventory_matches(uintptr_t,KilnEntity e,uint32_t,bool* match,unsigned long*) { *match = e > 1023 && fixture::inventory.count(e); return true; }
}
namespace {
using namespace kitems;
PayOut* __fastcall pay(PayOut* out,void* tx,uint32_t entity,uint32_t item,uint32_t n) {
    if(tx!=fixture::expected_tx)fixture::wrong_tx=true;
    *out={};
    if(fixture::refuse==entity){out->error=0x0b;return out;}
    if(fixture::fail_take) RaiseException(0xe1230001,0,0,nullptr);
    auto& count=fixture::inventory[entity][item]; const auto take=std::min(n,count); count-=take;
    if(fixture::tx_view.count(entity)) fixture::tx_view[entity][item]-=std::min(take,fixture::tx_view[entity][item]);
    if(take)fixture::taken.emplace_back(entity,take);
    out->left=n-take;return out;
}
PtrSize* __fastcall component(PtrSize* out,uintptr_t,uint32_t,uint32_t) { *out={}; return out; }
bool KILN_CALL tick_thread() {return true;}
void* g_bases_ptr = nullptr;
uintptr_t KILN_CALL singleton(uintptr_t,KilnStr name) {
    return std::string_view(name.ptr,name.len)=="PlayerBasesSystemServer"?reinterpret_cast<uintptr_t>(g_bases_ptr):0;
}
KilnResult KILN_CALL session(KilnSessionInfo* info) {info->state=KILN_SESSION_DEDICATED;return KILN_OK;}
// the program context: qword 0 = the job; JobStorage(job) = the storage, its kind byte at +8
uint8_t g_storage[16] = {};
void* __fastcall job_storage(void*) { g_storage[8] = fixture::world_kind; return g_storage; }
uint32_t* current_entity(void*, uint32_t* out) { *out = fixture::current; return out; }
// kiln.hooks and kiln.caps stand-ins: mid2 and raw_detour record their targets, code_state answers FOREIGN for an
// address in `foreign`, report records the capability rows lowered
std::map<uintptr_t,int> mid2_links, raw_links;
std::vector<uintptr_t> foreign;
using ApplyFn = bool(__fastcall*)(void*,void*,uint8_t);
bool seen_scope = false; int apply_calls = 0;
bool __fastcall fake_apply(void*,void*,uint8_t) { seen_scope = t_build_pay; ++apply_calls; return true; }
// the build menu's gatherer: rebuilds the groups in `rebuild` from `game` (group -> item -> the game's count)
std::set<uint32_t> rebuild;
std::map<uint32_t,std::map<uint32_t,uint64_t>> game;
int rows_calls = 0;
uint64_t __fastcall fake_rows(void* state,void*,uint8_t) {
    ++rows_calls;
    for (uint32_t g : rebuild) {
        auto* group=static_cast<uint8_t*>(state)+g*kRowGroupStride; auto* rows=rd<uint8_t*>(group,0); const uint64_t n=rd<uint64_t>(group,8);
        for (uint64_t i=0;i<n;++i) wr<uint64_t>(rows+i*kRowStride,kRowStock,game[g][rd<uint32_t>(rows+i*kRowStride,0)]);
    }
    return 7;
}
uintptr_t apply_caller = 0;
KilnResult KILN_CALL mid2(KilnOwner,uintptr_t target,const KilnHookOrder*,KilnMidFn,void*,KilnHandle* h) { ++mid2_links[target]; *h = 100 + target; return KILN_OK; }
KilnResult KILN_CALL raw(KilnOwner,uintptr_t target,const KilnHookOrder*,void*,void** next,KilnHandle* h) {
    ++raw_links[target]; *next = target == g_build_rows_fn ? reinterpret_cast<void*>(&fake_rows) : reinterpret_cast<void*>(&fake_apply);
    *h = 200 + target; return KILN_OK;
}
uintptr_t KILN_CALL caller(KilnHandle) { return apply_caller; }
KilnResult KILN_CALL revoke(KilnHandle) { return KILN_OK; }
KilnResult KILN_CALL code_state(uintptr_t at,size_t n,uint32_t* state,uint64_t* generation) {
    *state=KILN_CODE_ORIGINAL; *generation=0;
    for (uintptr_t f : foreign) if (f >= at && f - at < n) *state = KILN_CODE_FOREIGN;
    return KILN_OK;
}
KilnHooksApi hooks = [] { KilnHooksApi t{}; t.header = {sizeof t, KILN_HOOKS_VERSION}; t.mid2 = &mid2; t.raw_detour = &raw; t.caller = &caller;
                          t.revoke = &revoke; t.code_state = &code_state; return t; }();
std::map<std::string,uint32_t> lowered;
KilnResult KILN_CALL report(KilnOwner,KilnStr feature,uint32_t,uint32_t reason,KilnStr) { lowered[std::string(feature.ptr, feature.len)] = reason; return KILN_OK; }
KilnCapsApi caps = [] { KilnCapsApi t{}; t.header = {sizeof t, KILN_CAPS_VERSION}; t.report = &report; return t; }();
const void* KILN_CALL service(KilnStr name,uint32_t) {
    const std::string s(name.ptr, name.len);
    return s == KILN_HOOKS_SERVICE ? static_cast<const void*>(&hooks) : s == KILN_CAPS_SERVICE ? static_cast<const void*>(&caps) : nullptr;
}
uint32_t KILN_CALL side() { return KILN_SIDE_CLIENT; }  // a hosting client: its server world and its client world
// review of 31.12: where the player stands (kiln.world's position) and the game's base lookup (base 0, or none)
bool player_placed = false, in_base = true;
KilnResult KILN_CALL world_position(KilnEntity e, KilnVec3* out) {
    if (!player_placed || e != 1) return KILN_ERR_NOT_FOUND;
    *out = {1000, 50, 1000};
    return KILN_OK;
}
BaseHit* __fastcall base_at(BaseHit* out, void*, const float*) { *out = {}; out->error = in_base ? 0 : 0x1f; out->index = 0; return out; }
}
int main() {
    using namespace kitems;
    int checks=0,failed=0;
#define CHECK(x) do {++checks;if(!(x)){++failed;std::printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
    auto logged=[&](const char* s) { for(const auto& l:logs) if(l.find(s)!=std::string::npos) return true; return false; };
    KilnApi api{}; api.size=sizeof api;api.session=&session;api.on_tick_thread=&tick_thread;api.singleton=&singleton;
    api.service=&service;api.side=&side; g_api=&api;
    G.read=G.craft_ok=G.registered_ok=G.outputs_ok=true;G.pay=&pay;
    E.get_component=&component;E.job_storage=&job_storage;H_FACTORY=99;
    g_lock_current=&current_entity;
    for (LogLimit* l : {&g_log_build,&g_log_build_snap,&g_log_build_pay}) l->budget=100000;
    uint8_t infos[0xa0*2]={};wr(infos,0,uint32_t{1});infos[0x9d]=2;
    uint8_t bases[0x400]={};wr(bases,0x10,infos);g_srv_bases.store(bases);g_bases_ptr=bases;
    constexpr uint32_t PLAYER=1, CRAFT=2100, BUILD=2200, BUILD2=2300, STONE=0x77, WOOD=0x88;
    // the symbols, at made-up addresses (the sigdb rows are checked on the exe copies by build_sources_sigdb_test.py)
    g_build={0x140099a50,0x140209210,0x14022d480,0x1400c4b50,0x14022d562,0x140199c80};g_build_feat=true;
    auto add=[&](uint32_t container,uint32_t flags,uint32_t filter=0,std::vector<uint32_t> items={}) {
        KilnCraftSourceRequest r{sizeof r,PLAYER,container,0,filter,static_cast<uint32_t>(items.size()),items.data(),flags,0};
        uint64_t h=0; return t_add_source(10,&r,&h)==KILN_OK ? h : 0;
    };
    fixture::inventory={{PLAYER,{}},{CRAFT,{{STONE,50}}},{BUILD,{{STONE,30},{WOOD,4}}},{BUILD2,{{STONE,5}}}};
    // ---- the cursor's hook: a fake cursor query (material at [[rsi+0x68]]) and context ([rdi] = the program context) ----
    uint32_t material=STONE; uint8_t query[0x80]={}; wr(query,0x68,&material);
    void* job=reinterpret_cast<void*>(0x1234); void* ctx=&job; void* cursor[2]={ctx,nullptr};
    auto look=[&](uint8_t world,uint32_t base,uint32_t bag,uint32_t need) {
        fixture::world_kind=world;
        KilnRegisters r{};r.rax=base;r.rbx=bag;r.rbp=need;r.rsi=reinterpret_cast<uintptr_t>(query);r.rdi=reinterpret_cast<uintptr_t>(cursor);
        logs.clear();hk_build_cursor(r);return static_cast<uint32_t>(r.rax);
    };
    CHECK(look(0,10,5,64)==10 && logs.empty());           // no building link: the game's count, no line
    const uint64_t craft_only=add(CRAFT,0);
    build_cache_refresh(1);
    CHECK(look(0,10,5,64)==10 && logs.empty());           // a crafting-only link never counts for building
    const uint64_t b1=add(BUILD,KILN_CRAFT_SOURCE_BUILDING), b2=add(BUILD2,KILN_CRAFT_SOURCE_BUILDING,1,{STONE});
    CHECK(b1 && b2 && craft_only);
    build_cache_refresh(2);
    CHECK(look(0,10,5,40)==45 && logged("server world's material check of item 0x00000077 (player 1) counts the building links: bag 5 + base 10 + linked 35, needs 40"));
    CHECK(look(0,10,5,51)==10 && logged("refused: short count: bag 5 + base 10 + linked 35, needs 51"));  // 50 < 51: the game's own verdict
    CHECK(look(0,40,30,64)==40 && logs.empty());           // the bag and the base cover it: never asked
    CHECK(look(0,10,5,0)==10);                             // nothing needed
    material=WOOD;CHECK(look(0,0,0,4)==4);material=STONE;  // BUILD2's filter allows stone only; BUILD holds 4 wood
    fixture::current=2;CHECK(look(0,10,5,40)==10);fixture::current=1;  // another player: not this player's links
    CHECK(look(1,10,5,40)==10);                            // a prediction world: the game's
    infos[0x9d]=0;g_build_dirty.store(true);build_cache_refresh(3);
    CHECK(look(0,10,5,40)==10);                            // no CanAccessInventories: the links count nothing
    infos[0x9d]=2;g_build_dirty.store(true);build_cache_refresh(4);
    // a Kiln client's own world: only the server's snapshot
    CHECK(look(2,10,5,40)==10);                            // no snapshot yet
    KilnStationStack rows[2]={{STONE,35},{WOOD,4}};
    CHECK(t_set_build_stock(1,rows,2)==KILN_OK && logged("the server counts building materials from linked sources: 2 item row(s)"));
    CHECK(look(2,10,5,40)==45);
    material=0x99;CHECK(look(2,0,0,1)==0);material=STONE;  // no row: the links hold none
    KilnStationStack bad[1]={{0,3}};
    CHECK(t_set_build_stock(1,bad,1)==KILN_ERR_INVALID_ARGUMENT && t_set_build_stock(2,nullptr,0)==KILN_ERR_INVALID_ARGUMENT);
    CHECK(t_set_build_stock(1,nullptr,1)==KILN_ERR_INVALID_ARGUMENT && t_set_build_stock(1,rows,KILN_STATIONS_MAX_CRAFT_STOCK+1)==KILN_ERR_INVALID_ARGUMENT);
    CHECK(look(2,10,5,40)==45);                            // a refused update keeps the snapshot
    {KilnWorldEvent stop{sizeof stop,KILN_WORLD_STOPPING,KILN_WORLD_CLIENT,1,0};client_world(&stop,nullptr);}
    CHECK(look(2,10,5,40)==10 && !g_build_on.load());      // the client world stopped: the snapshot is gone
    // a query that cannot be read: the game's count, and the hooks stand down (the fault guard; one line)
    {KilnRegisters r{};r.rax=1;r.rbx=0;r.rbp=9;r.rsi=0x10;r.rdi=reinterpret_cast<uintptr_t>(cursor);logs.clear();hk_build_cursor(r);
     CHECK(r.rax==1 && g_build_off.load() && logged("material hook faulted"));
     r.rsi=reinterpret_cast<uintptr_t>(query);r.rax=10;r.rbx=5;r.rbp=40;fixture::world_kind=0;hk_build_cursor(r);CHECK(r.rax==10);  // off: the game's
     g_build_off.store(false);}
    // ---- review of 31.12: a building link to a Magic chest of the base the player stands in counts once ----
    // The cursor adds the links to the game's own count of the base (eax), and the placement pays the base list first and
    // leaves it out of the links: a player who links every chest (Streamlined Logistics) had the base's chest counted
    // twice, a green cursor and a build menu over the real count, and then a placement that came up short.
    {
        KilnWorldApi world{}; world.position = &world_position; g_w = &world;
        G.base_at = &base_at; G.bases_count = 0x100; G.stride = 0x80; G.stock_items = 0x108; G.stock_items_count = 0x110;
        G.stock_water = 0x118; G.stock_water_count = 0x120; G.aabb_y = 0x134; G.zone_mask_rel = 0x20;
        uint32_t chests[1] = {BUILD};  // the base's stock list names the linked chest BUILD (30 stone, 4 wood)
        wr(bases, 0x100, uint32_t{1}); wr(bases, 0x108, static_cast<uint32_t*>(chests)); wr(bases, 0x110, uint64_t{1});
        auto building = [&] {
            KilnStationStack out[8] = {}; uint32_t n = 0;
            std::map<uint32_t, uint32_t> m;
            if (t_source_stock(PLAYER, KILN_CRAFT_SOURCE_STOCK_BUILDING, out, 8, &n) == KILN_OK)
                for (uint32_t i = 0; i < n; ++i) m[out[i].item] = out[i].count;
            return m;
        };
        CHECK((building() == std::map<uint32_t, uint32_t>{{STONE, 35}, {WOOD, 4}}));  // nowhere: every building link
        player_placed = true; g_build_dirty.store(true); build_cache_refresh(5);
        CHECK((building() == std::map<uint32_t, uint32_t>{{STONE, 5}}));              // in the base: BUILD is the base's count
        CHECK(look(0,10,5,40)==10 && logged("refused: short count: bag 5 + base 10 + linked 5, needs 40"));
        CHECK(look(0,10,5,20)==15);                                                     // BUILD2's 5 stone still count
        material=WOOD;CHECK(look(0,0,0,4)==0);material=STONE;                          // BUILD's wood is the base's now
        in_base = false; g_build_dirty.store(true); build_cache_refresh(6);
        CHECK((building() == std::map<uint32_t, uint32_t>{{STONE, 35}, {WOOD, 4}}));  // in no base: every link again
        CHECK(look(0,10,5,40)==45);
        in_base = true; player_placed = false; g_build_dirty.store(true); build_cache_refresh(7);
        CHECK(look(0,10,5,40)==45);                                                     // no position: nothing left out
        g_w = nullptr; G.base_at = nullptr; G.bases_count = G.stride = G.stock_items = G.stock_items_count = 0;
        G.stock_water = G.stock_water_count = G.aabb_y = G.zone_mask_rel = 0; std::memset(bases + 0x100, 0, 0x100);
        g_build_dirty.store(true); build_cache_refresh(8);
    }
    // ---- the payment's scope: only the blueprint program's own take ----
    apply_caller=g_build.blueprint_ret;
    {
        h_build_apply.install(g_api,g_owner,g_build.apply,reinterpret_cast<void*>(&hk_build_apply),"build/ApplyMaterials");
        uint8_t list[16]={};
        CHECK(hk_build_apply(nullptr,list,1) && seen_scope && !t_build_pay);  // inside: set; after: back
        CHECK(hk_build_apply(nullptr,list,0) && !seen_scope);                 // a give (refund) is never paid from links
        apply_caller=0x1400c6be9;CHECK(hk_build_apply(nullptr,list,1) && !seen_scope);  // place_prop's take: the game's
        apply_caller=g_build.blueprint_ret;g_build_off.store(true);CHECK(hk_build_apply(nullptr,list,1) && !seen_scope);
        g_build_off.store(false);
        // another module's code made the call (Building Tools 0.34.0 routes the blueprint program's payment through its own
        // code): with no game call of ApplyMaterials behind it on this stack, the take is the game's, and kiln.log says so
        apply_caller=0x7ff800001234;
        CHECK(hk_build_apply(nullptr,list,1) && !seen_scope && logged("could not be traced to the game's call behind it"));
        apply_caller=g_build.blueprint_ret;
        g_build_off.store(false);h_build_apply={};raw_links.clear();
    }
    // ---- the build menu's material counts: bag + base + links, once, whatever the gatherer rebuilt ----
    {
        g_build_ui_feat=true;g_build_rows_fn=0x14040e5d0;g_build_row_add=0x14040c010;g_build_stock_rows_ret=0x14040ee4b;
        g_build_rows_body=g_build_rows_fn+0x23;g_build_rows_done=g_build_rows_fn+0xa8a;
        // the gatherer as the game runs it with Kiln's two halves in: its body between them, this call's frame as its rsp
        auto gather=[&](uint8_t* s,uint8_t only) {
            volatile uint8_t mark=0;const auto frame=reinterpret_cast<uintptr_t>(&mark);
            build_rows_enter(s,frame);const uint64_t r=fake_rows(s,nullptr,only);build_rows_leave(frame,s);return r;
        };
        std::vector<uint8_t> state(kRowGroups*kRowGroupStride+0x10,0);
        uint8_t g1[3*kRowStride]={}, g3[1*kRowStride]={};
        wr(g1,0,STONE);wr(g1,kRowStride,WOOD);wr(g1,2*kRowStride,uint32_t{0x99});wr(g3,0,STONE);
        wr(state.data()+1*kRowGroupStride,0,static_cast<uint8_t*>(g1));wr(state.data()+1*kRowGroupStride,8,uint64_t{3});
        wr(state.data()+3*kRowGroupStride,0,static_cast<uint8_t*>(g3));wr(state.data()+3*kRowGroupStride,8,uint64_t{1});
        auto stock=[&](uint8_t* rows,int i) {return rd<uint64_t>(rows+i*kRowStride,kRowStock);};
        KilnStationStack snap[2]={{STONE,35},{WOOD,4}};
        CHECK(t_set_build_stock(1,snap,2)==KILN_OK);
        game={{1,{{STONE,10},{WOOD,0},{0x99,3}}},{3,{{STONE,2}}}};rebuild={1,3};
        CHECK(gather(state.data(),0)==7);
        CHECK(stock(g1,0)==45 && stock(g1,1)==4 && stock(g1,2)==3 && stock(g3,0)==37);  // every group holding a linked item
        rebuild={};                                                                      // currentOnly: nothing rebuilt
        gather(state.data(),1);
        CHECK(stock(g1,0)==45 && stock(g1,1)==4 && stock(g3,0)==37);                    // raised once, not twice
        game[1][STONE]=12;rebuild={1};                                                   // the bag gained 2 stone
        gather(state.data(),1);
        CHECK(stock(g1,0)==47 && stock(g3,0)==37);
        wr<uint64_t>(g3,kRowStock,50);rebuild={};                                       // the game changed a kept row itself
        gather(state.data(),1);
        CHECK(stock(g3,0)==85);                                                          // its value is the game's, plus the links
        snap[0].count=5;t_set_build_stock(1,snap,2);rebuild={1,3};game[3][STONE]=2;
        gather(state.data(),0);
        CHECK(stock(g1,0)==17 && stock(g3,0)==7);                                       // a new snapshot
        t_set_build_stock(0,nullptr,0);rebuild={};
        gather(state.data(),1);
        CHECK(stock(g1,0)==12 && stock(g1,1)==0 && stock(g3,0)==2 && g_build_marks.empty());  // off: the game's counts again
        CHECK(rows_calls==6);
        // a state that cannot be read: the original still runs, the menu keeps the game's counts from then on
        t_set_build_stock(1,snap,2);logs.clear();
        std::vector<uint8_t> broken(kRowGroups*kRowGroupStride,0);wr(broken.data(),0,reinterpret_cast<uint8_t*>(0x10));wr(broken.data(),8,uint64_t{1});
        rebuild={};CHECK(gather(broken.data(),1)==7 && g_build_rows_off.load() && logged("material rows could not be read"));
        gather(state.data(),1);CHECK(stock(g1,0)==12);
        g_build_rows_off.store(false);
        // a second half whose first did not run (the hooks went in while a call was inside): the game's counts, nothing raised
        {volatile uint8_t mark=0;const auto frame=reinterpret_cast<uintptr_t>(&mark);t_set_build_stock(1,snap,2);rebuild={1};game[1][STONE]=12;
         fake_rows(state.data(),nullptr,1);build_rows_leave(frame,state.data());CHECK(stock(g1,0)==12 && t_build_rows.depth()==0 && g_build_marks.empty());}
        t_set_build_stock(0,nullptr,0);raw_links.clear();g_build_marks.clear();
    }
    // ---- install on demand: the server once a Building link exists, a client once its snapshot is on ----
    g_build_apply_on.store(false);g_build_cursor_on.store(false);mid2_links.clear();raw_links.clear();
    build_client_pre();CHECK(mid2_links.empty());                       // no snapshot: nothing goes in
    CHECK(t_set_build_stock(1,rows,2)==KILN_OK);logs.clear();
    build_client_pre();CHECK(mid2_links[g_build.cursor_ret]==1 && mid2_links[g_build_rows_body]==1 && mid2_links[g_build_rows_done]==1 &&
                             logged("this client's build cursor counts"));
    // XHL, 2026-11-25: kiln-items hooked the gatherer's entry, which Building Tools 0.34.0 finds by its first 35 bytes (and
    // then the bytes and calls up to + 0xa7a), so its full material list and partial area actions stayed off. Now inside it.
    CHECK(!raw_links.count(g_build_rows_fn) && !mid2_links.count(g_build_rows_fn));
    for (const auto& [at, n] : mid2_links) CHECK(n && (at < g_build_rows_fn || at >= g_build_rows_fn + 35));
    for (uintptr_t bt : {0x194, 0x705, 0x721, 0x78f, 0x867, 0x876, 0x89d, 0xa76})
        for (const auto& [at, n] : mid2_links) CHECK(at + 5 <= g_build_rows_fn + bt || at >= g_build_rows_fn + bt + 7);
    CHECK(g_build_rows_on.load() && logged("the build menu's material counts include the building links"));
    build_server_pre(5);CHECK(raw_links[g_build.apply]==1 && mid2_links[g_build.cursor_ret]==1 && mid2_links[g_build_rows_body]==1);  // shared
    build_server_pre(6);CHECK(raw_links[g_build.apply]==1 && mid2_links[g_build.cursor_ret]==1);  // once
    t_set_build_stock(0,nullptr,0);
    // ---- Building Tools 0.34.0 redirects the blueprint program's payment call and the cursor's stock count (their rel32
    // only): the hooks follow the game's call, they do not stand down ----
    g_build_apply_on.store(false);g_build_cursor_on.store(false);h_build_cursor={};h_build_apply={};mid2_links.clear();raw_links.clear();
    lowered.clear();foreign={g_build.blueprint_ret-3,g_build.cursor_ret-2};logs.clear();
    build_server_pre(9);
    CHECK(!g_build_off.load() && g_build_apply_on.load() && g_build_cursor_on.load() && raw_links[g_build.apply]==1 &&
          mid2_links[g_build.cursor_ret]==1 && !lowered.count("crafting.build_sources"));
    CHECK(logged("build: the blueprint program's payment call (rva") && logged("build: the build cursor's stock-count call (rva") &&
          logged("is redirected to another module's code: the building hooks follow the game's call through it"));
    foreign.clear();
    // ---- the stand-down on foreign code (Cross-Base Storage's build check, say) ----
    // the menu's gatherer changed by another module: only the menu's counts stand down, the cursor stays
    g_build_rows_on.store(false);h_build_rows_body={};h_build_rows_done={};raw_links.clear();mid2_links.clear();foreign={g_build_rows_fn+8};logs.clear();
    build_rows_install();
    CHECK(g_build_rows_off.load() && !g_build_off.load() && raw_links.empty() && mid2_links.empty() && lowered["crafting.build_ui"]==KILN_CAP_REASON_FAULTED);
    CHECK(logged("build menu's counts of linked sources stand down: the build menu's material gatherer code state FOREIGN"));
    foreign.clear();g_build_rows_off.store(false);
    g_build_apply_on.store(false);g_build_cursor_on.store(false);h_build_cursor={};h_build_apply={};mid2_links.clear();raw_links.clear();
    foreign={g_build.stock_fn+0x10};logs.clear();
    build_server_pre(10);
    CHECK(g_build_off.load() && !g_build_cursor_on.load() && mid2_links.empty());
    CHECK(logged("build: the build cursor's count of linked sources stands down: the base stock count (build check) code state FOREIGN"));
    CHECK(lowered["crafting.build_sources"]==KILN_CAP_REASON_FAULTED);
    std::printf("build sources: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
