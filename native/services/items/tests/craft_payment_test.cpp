// SPDX-License-Identifier: MIT
// The actual RecipePaid adapter on fake server transactions. No game, no live world.
#include "../stations.cpp"
#include <cstdarg>
#include <map>
#include <string>
namespace fixture {
std::map<uint32_t,std::map<uint32_t,uint32_t>> inventory;
bool server_storage = true, fail_take = false;
uint8_t stock_error = 0x1f;
std::vector<uint32_t> paid;
void* bases=nullptr;
bool in_base=false;
void* expected_tx=nullptr;
bool wrong_tx=false;
bool production_station=false, component_fault=false;
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
std::vector<std::string> logs; // every kiln.log line the adapter wrote
void items_log(int,const char* fmt,...) {
    char b[1024]; va_list a; va_start(a,fmt); std::vsnprintf(b,sizeof b,fmt,a); va_end(a); logs.emplace_back(b);
}
uint64_t current_step() { return 1; }
const KilnApi* items_api() { return g_api; }
KilnOwner items_owner() { return 10; }
bool access_from_tx(void*,Access* out) { *out = {}; return fixture::server_storage; }
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
BaseHit* __fastcall base_at(BaseHit* hit,void*,const float*) { *hit={}; hit->error=fixture::in_base?0:0x1f;return hit; }
void __fastcall base_stock(StockErr* err,StockLists* stock,uint64_t,uint64_t,void*,uint32_t) { *err={};err->error=fixture::stock_error;*stock={}; }
PayOut* __fastcall pay(PayOut* out,void* tx,uint32_t entity,uint32_t item,uint32_t n) {
    if(tx!=fixture::expected_tx)fixture::wrong_tx=true;
    if(fixture::fail_take && entity==2000) RaiseException(0xe1230001,0,0,nullptr);
    auto& count=fixture::inventory[entity][item]; const auto take=std::min(n,count); count-=take;
    if(take)fixture::paid.push_back(entity);
    *out={};out->left=n-take;return out;
}
KilnResult KILN_CALL position(KilnEntity,KilnVec3* out) {*out={};return KILN_OK;}
PtrSize* __fastcall component(PtrSize* out,uintptr_t,uint32_t,uint32_t entity) {
    if(fixture::component_fault)RaiseException(0xe1230001,0,0,nullptr);
    static uint8_t factory[0x68]={};
    *out={};
    if(entity==2000 && fixture::production_station) {out->ptr=factory;out->size=sizeof factory;}
    return out;
}
bool KILN_CALL tick_thread() {return true;}
uintptr_t KILN_CALL singleton(uintptr_t,KilnStr name) {
    return std::string_view(name.ptr,name.len)=="PlayerBasesSystemServer"?reinterpret_cast<uintptr_t>(fixture::bases):0;
}
KilnResult KILN_CALL session(KilnSessionInfo* info) {info->state=KILN_SESSION_DEDICATED;return KILN_OK;}
}
int main() {
    using namespace kitems;
    int checks=0,failed=0;
#define CHECK(x) do {++checks;if(!(x)){++failed;std::printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
    KilnApi api{}; api.session=&session;api.on_tick_thread=&tick_thread;api.singleton=&singleton; g_api=&api;
    G.read=G.craft_ok=G.registered_ok=G.outputs_ok=true;G.base_at=&base_at;G.base_stock=&base_stock;G.pay=&pay;G.recipe_paid=0x1234;
    E.get_component=&component;H_FACTORY=99;
    uint8_t infos[0xa0*2]={};wr(infos,0,uint32_t{1});infos[0x9d]=2;
    uint8_t bases[0x400]={};wr(bases,0x10,infos);g_srv_bases.store(bases);fixture::bases=bases;
    uint8_t tx[0x100]={};wr(tx,0x70,bases);fixture::expected_tx=tx;
    uint8_t cost[24]={};wr(cost,0x10,uint32_t{7});wr(cost,0x14,uint32_t{3});
    struct {void* ptr;uint64_t n;} costs{cost,1};
    fixture::inventory={{1,{{7,1}}},{2000,{{7,2}}}};
    uint64_t handle=0;
    KilnCraftSourceRequest request{sizeof request,1,2000,0,0,0,nullptr};
    g_srv_bases.store(nullptr);uint64_t early=0;
    CHECK(t_add_source(10,&request,&early)==KILN_OK && early!=0); // current server singleton, before the first cached Pre
    if(early)t_remove_source(10,early);
    g_srv_bases.store(bases);
    CHECK(t_add_source(10,&request,&handle)==KILN_OK && handle!=0);
    KilnStationStack stocks[2]{};uint32_t n=0;
    CHECK(t_source_stock(1,0,stocks,2,&n)==KILN_OK && n==1 && stocks[0].item==7 && stocks[0].count==2);
    CHECK(t_source_stock(1,0,nullptr,0,&n)==KILN_ERR_BUFFER_TOO_SMALL && n==1);
    CHECK(t_source_stock(1,KILN_CRAFT_SOURCE_STOCK_ALL,stocks,2,&n)==KILN_OK && n==1 && stocks[0].count==2);
    CHECK(t_source_stock(1,2,stocks,2,&n)==KILN_ERR_INVALID_ARGUMENT);
    KilnWorldApi world{};world.position=&position;g_w=&world;
    fixture::in_base=true;uint32_t base_chest=2000;
    G.stock_items=0x20;G.stock_items_count=0x28;G.stock_water=0x30;G.stock_water_count=0x38;G.aabb_y=0x80;G.zone_mask_rel=0x20;
    wr(bases,0,uint32_t{1});wr(bases,0x20,&base_chest);wr(bases,0x28,uint64_t{1});
    CHECK(t_source_stock(1,0,stocks,2,&n)==KILN_OK && n==0); // already present in crafting base stock
    CHECK(t_source_stock(1,KILN_CRAFT_SOURCE_STOCK_ALL,stocks,2,&n)==KILN_OK && n==1 && stocks[0].count==2); // tint still needs it
    fixture::in_base=false;g_w=nullptr;wr(bases,0,uint32_t{0});
    auto bad=request;bad.container=2;uint64_t rejected=0;
    CHECK(t_add_source(10,&bad,&rejected)==KILN_ERR_INVALID_ARGUMENT && !rejected);
    bad.container=9999;
    CHECK(t_add_source(10,&bad,&rejected)==KILN_ERR_INVALID_ARGUMENT && !rejected);
    CHECK(t_remove_source(11,handle)==KILN_ERR_NOT_FOUND);
    fixture::production_station=true;
    CHECK(t_add_source(10,&request,&rejected)==KILN_ERR_INVALID_ARGUMENT && !rejected);
    CHECK(t_source_stock(1,0,stocks,2,&n)==KILN_OK && n==0); // a former chest may now name a station
    CHECK(t_source_stock(1,KILN_CRAFT_SOURCE_STOCK_ALL,stocks,2,&n)==KILN_OK && n==0);
    uint32_t station_code=99;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&station_code)==0 && fixture::paid.empty());
    CHECK(t_set_output_source(2000,1)==KILN_OK && sources_copy()==std::vector<uint32_t>({2000}));
    CHECK(t_set_output_source(2000,0)==KILN_OK && sources_copy().empty());
    fixture::production_station=false;fixture::component_fault=true;
    CHECK(t_add_source(10,&request,&rejected)==KILN_ERR_INVALID_ARGUMENT && !rejected);
    CHECK(t_source_stock(1,0,stocks,2,&n)==KILN_OK && n==0); // unknown component kind fails closed
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&station_code)==0 && fixture::paid.empty());
    fixture::component_fault=false;
    uint32_t code=99;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==1 && code==0);
    CHECK(fixture::paid==std::vector<uint32_t>({1,2000}) && !fixture::wrong_tx);
    auto reset=[&] {fixture::inventory={{1,{{7,1}}},{2000,{{7,2}}}};fixture::paid.clear();code=99;};
    reset();wr(tx,0x70,uint64_t{0});
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==1 && code==0); // owner-only transaction may omit base words
    wr(tx,0x70,bases);
    reset();tx[0xb1]=2;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==1 && code==0); // no base, error 0x1f
    reset();fixture::stock_error=0x16;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==1 && code==0); // base check fails, independent right succeeds
    reset();infos[0x9d]=0;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==0 && fixture::paid.empty());
    reset();infos[0x9d]=2;
    CHECK(craft_pay_impl(tx,&costs,2,1,nullptr,0x1234,&code)==0 && fixture::paid.empty());
    reset();wr(infos,4,uint32_t{99});
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==0 && fixture::paid.empty()); // reused player slot, another login
    reset();
    CHECK(t_source_stock(1,0,stocks,2,&n)==KILN_OK && n==0);
    wr(infos,4,uint32_t{0});
    reset();fixture::server_storage=false;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==0 && fixture::paid.empty());
    fixture::server_storage=true;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x9999,&code)==0 && fixture::paid.empty());
    reset();fixture::fail_take=true;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==1 && code==4); // caller must rollback this transaction
    fixture::fail_take=false;
    // #132: a craft at a crafting station (the decoration table's menu sends the same CraftRecipe; kiln.gameplay's craft
    // hook names the station for the call) pays from the player's linked source the same way, all or nothing
    reset();t_craft_station=3000;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==1 && code==0 && fixture::paid==std::vector<uint32_t>({1,2000}));
    // the station being crafted at is never a source, even when a mod linked it
    fixture::inventory={{1,{{7,1}}},{2000,{{7,0}}},{3000,{{7,5}}}};fixture::paid.clear();code=99;
    uint64_t station_link=0;KilnCraftSourceRequest at_station{sizeof at_station,1,3000,0,0,0,nullptr};
    CHECK(t_add_source(10,&at_station,&station_link)==KILN_OK && station_link!=0);
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==0 && fixture::paid.empty());
    t_craft_station=0;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==1 && code==0 && fixture::paid==std::vector<uint32_t>({1,3000}));
    g_registered.remove(10,station_link);
    reset();g_registered.remove(10,handle);
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==0 && fixture::paid.empty());
    // #132 follow-up (hosting client): the client world's own dry runs (RecipeApply on its own transaction, every recipe
    // the crafting menu counts) must not use up the lines that say why the server world's craft was left to the game.
    auto logged=[&](const char* s) { for(const auto& l:logs) if(l.find(s)!=std::string::npos) return true; return false; };
    reset();CHECK(t_add_source(10,&request,&handle)==KILN_OK && handle!=0);
    uint8_t client_bases[0x400]={};uint8_t dry[0x100]={};wr(dry,0x70,client_bases);
    for(int i=0;i<20;++i) CHECK(craft_pay_impl(dry,&costs,1,1,nullptr,0x1234,&code)==0);
    logs.clear();fixture::server_storage=false;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==0 && fixture::paid.empty());
    CHECK(logged("the server world's craft transaction"));
    fixture::server_storage=true;
    // one line per player craft, at its end, saying how far it got (kiln.gameplay's CraftRecipe hook brackets the call)
    reset();logs.clear();
    kiln_stations_set_craft_context(3000,1);
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==1 && code==0);
    kiln_stations_set_craft_context(0,0);
    kiln_stations_craft_done(1,3000,0x77,0);
    CHECK(logged("player 1's craft of recipe 0x00000077 at station 3000: paid 2 item(s)"));
    // refused before its payment: the game never asked (a requirement, or the station's props)
    logs.clear();
    kiln_stations_set_craft_context(3000,1);kiln_stations_set_craft_context(0,0);kiln_stations_craft_done(1,3000,0x77,4);
    CHECK(logged("refused without reaching the recipe payment"));
    // the payment saw another login's links
    reset();logs.clear();wr(infos,4,uint32_t{99});
    kiln_stations_set_craft_context(3000,1);
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==0 && fixture::paid.empty());
    kiln_stations_set_craft_context(0,0);kiln_stations_craft_done(1,3000,0x77,4);
    CHECK(logged("earlier login"));
    wr(infos,4,uint32_t{0});
    // short even with the linked source: nothing taken, the line says by how much
    reset();logs.clear();fixture::inventory={{1,{{7,0}}},{2000,{{7,1}}}};
    kiln_stations_set_craft_context(3000,1);
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==0 && fixture::paid.empty());
    kiln_stations_set_craft_context(0,0);kiln_stations_craft_done(1,3000,0x77,4);
    CHECK(logged("short by 2"));
    // a factory's own cycle and a craft outside any context say nothing
    reset();logs.clear();
    kiln_stations_set_craft_context(3000,1);kiln_stations_set_craft_context(0,0);kiln_stations_craft_done(3000,3000,0x77,0);
    kiln_stations_craft_done(1,3000,0x77,0);
    CHECK(!logged("'s craft of recipe"));
    g_registered.remove(10,handle);
    // a player with nothing linked and no station opted in: the game's craft, no line
    logs.clear();
    kiln_stations_set_craft_context(3000,1);kiln_stations_set_craft_context(0,0);kiln_stations_craft_done(1,3000,0x77,4);
    CHECK(!logged("'s craft of recipe"));
    std::printf("craft payment: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
