// SPDX-License-Identifier: MIT
// The actual RecipePaid adapter on fake server transactions. No game, no live world.
#include "../stations.cpp"
#include <map>
namespace fixture {
std::map<uint32_t,std::map<uint32_t,uint32_t>> inventory;
bool server_storage = true, fail_take = false;
uint8_t stock_error = 0x1f;
std::vector<uint32_t> paid;
}
namespace kitems {
Engine E{};
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
uintptr_t items_server_world() { return 1; }
void items_log(int,const char*,...) {}
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
BaseHit* __fastcall base_at(BaseHit* hit,void*,const float*) { *hit={}; hit->error=0x1f;return hit; }
void __fastcall base_stock(StockErr* err,StockLists* stock,uint64_t,uint64_t,void*,uint32_t) { *err={};err->error=fixture::stock_error;*stock={}; }
PayOut* __fastcall pay(PayOut* out,void*,uint32_t entity,uint32_t item,uint32_t n) {
    if(fixture::fail_take && entity==2000) RaiseException(0xe1230001,0,0,nullptr);
    auto& count=fixture::inventory[entity][item]; const auto take=std::min(n,count); count-=take;
    if(take)fixture::paid.push_back(entity);
    *out={};out->left=n-take;return out;
}
KilnResult KILN_CALL session(KilnSessionInfo* info) {info->state=KILN_SESSION_DEDICATED;return KILN_OK;}
}
int main() {
    using namespace kitems;
    int checks=0,failed=0;
#define CHECK(x) do {++checks;if(!(x)){++failed;std::printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
    KilnApi api{}; api.session=&session; g_api=&api;
    G.craft_ok=G.registered_ok=G.outputs_ok=true;G.base_at=&base_at;G.base_stock=&base_stock;G.pay=&pay;G.recipe_paid=0x1234;
    uint8_t infos[0xa0*2]={};wr(infos,0,uint32_t{1});infos[0x9d]=2;
    uint8_t bases[0x20]={};wr(bases,0x10,infos);g_srv_bases.store(bases);
    uint8_t tx[0x100]={};wr(tx,0x70,bases);
    uint8_t cost[24]={};wr(cost,0x10,uint32_t{7});wr(cost,0x14,uint32_t{3});
    struct {void* ptr;uint64_t n;} costs{cost,1};
    fixture::inventory={{1,{{7,1}}},{2000,{{7,2}}}};
    auto handle=g_registered.add(10,1,2000,false,true,{});
    uint32_t code=99;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==1 && code==0);
    CHECK(fixture::paid==std::vector<uint32_t>({1,2000}));
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
    reset();fixture::server_storage=false;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==0 && fixture::paid.empty());
    fixture::server_storage=true;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x9999,&code)==0 && fixture::paid.empty());
    reset();fixture::fail_take=true;
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==1 && code==4); // caller must rollback this transaction
    fixture::fail_take=false;
    reset();g_registered.remove(10,handle);
    CHECK(craft_pay_impl(tx,&costs,1,1,nullptr,0x1234,&code)==0 && fixture::paid.empty());
    std::printf("craft payment: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
