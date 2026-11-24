// SPDX-License-Identifier: MIT
// #132: the actual scoped client RecipeApply/InventoryPay preview bridge on fake game functions. No game is run.
#include "../stations.cpp"
#include <string>
namespace fixture {
uintptr_t ret = 0;
bool initialized=true, pages_ok=true, observe_take=true, null_take=false, bad_remainder=false;
uint8_t native_error=0;
uint32_t code_state=KILN_CODE_ORIGINAL, installs=0, eligibility=0, after_payment=0, native_payment=4, payment_calls=0;
uint8_t storage[32]={}, recipe[0xb0]={};
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
bool read_pages(const Access&,KilnEntity,PageSet* p,bool* init) { *p={};p->n=1;p->p[0].entity=99;*init=fixture::initialized;return fixture::pages_ok; }
void player_guard(const Access&,KilnEntity,PlayerGuard*) { std::abort(); }
bool inventory_matches(uintptr_t,KilnEntity,uint32_t,bool*,unsigned long*) { std::abort(); }
}
namespace {
using namespace kitems;
void* __fastcall job_storage(void*) {return fixture::storage;}
uint16_t* __fastcall component_index(void*,const uint32_t*) {static uint16_t v=1;return &v;}
PtrSize* __fastcall read_other(PtrSize* out,void*,uint64_t,uint32_t) {static uint8_t inv[kInventorySize]={};*out={inv,sizeof inv};return out;}
void* __fastcall item_find(uint32_t id,void*) {return id==7||id==8?fixture::recipe:nullptr;}
void __fastcall resolve(const void*,void* vec,const void*,void*) {auto* v=static_cast<ResolveVec*>(vec);v->count=1;wr(v->inline_,0x10,uint32_t{7});wr(v->inline_,0x14,uint32_t{5});}
void* __fastcall recipe_find(uint32_t id,void*) {return id==7?fixture::recipe:nullptr;}
PayOut* __fastcall take(PayOut* out,void*,uint32_t,uint32_t,uint32_t count) {*out={};out->error=fixture::native_error;out->left=count+fixture::bad_remainder;return fixture::null_take?nullptr:out;}
uint32_t __fastcall apply(void* tx,const void*,const void* inputs,uint64_t,const void*,const uint32_t*,uint32_t owner,int32_t amount) {
    if(fixture::eligibility)return fixture::eligibility;
    ++fixture::payment_calls;
    PayOut out{};if(fixture::observe_take)hk_preview_take(&out,tx,owner,7,5);
    const uint32_t paid=craft_preview_impl(tx,const_cast<void*>(inputs),owner,amount,nullptr,g_preview_paid,fixture::native_payment);
    return paid?paid:fixture::after_payment;
}
KilnResult KILN_CALL raw(KilnOwner,uintptr_t target,const KilnHookOrder*,void*,void** next,KilnHandle* h) {
    ++fixture::installs;*next=reinterpret_cast<void*>(target);*h=1;return KILN_OK;
}
KilnResult KILN_CALL code_state(uintptr_t,size_t,uint32_t* out,uint64_t* generation) {*out=fixture::code_state;*generation=1;return KILN_OK;}
KilnResult KILN_CALL revoke(KilnHandle) {return KILN_OK;}
uintptr_t KILN_CALL caller(KilnHandle) {return fixture::ret;}
KilnHooksApi hooks=[] {KilnHooksApi t{};t.header={sizeof t,KILN_HOOKS_VERSION};t.raw_detour=&raw;t.caller=&caller;t.revoke=&revoke;t.code_state=&code_state;return t;}();
const void* KILN_CALL service(KilnStr,uint32_t) {return &hooks;}
}
int main() {
    using namespace kitems;
    int checks=0,failed=0;
#define CHECK(x) do {++checks;if(!(x)){++failed;std::printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
    KilnApi api{};api.size=sizeof api;api.service=&service;g_api=&api;g_owner=10;
    E.tx_ok=true;E.job_storage=&job_storage;E.component_map_find=&component_index;E.read_other=&read_other;
    G.resolve=&resolve;G.recipe_find=&recipe_find;E.item_find=&item_find;g_preview_paid=0x1234;g_client_recipes.store(reinterpret_cast<void*>(1));
    g_preview_apply=reinterpret_cast<uintptr_t>(&apply);g_preview_ret=0x5678;g_preview_pay=0x9876;g_preview_take=reinterpret_cast<uintptr_t>(&take);g_preview_feat=true;
    KilnStationStack stock[]={{7,5},{8,2}};CHECK(t_set_craft_stock(stock,2)==KILN_OK);
    // A foreign hook (including CBS at this site) or unknown code means no second patch and no attempt to enable.
    for(uint32_t state : {KILN_CODE_FOREIGN,KILN_CODE_UNKNOWN}) {
        fixture::code_state=state;g_preview_failed=false;preview_hook_on_demand();CHECK(fixture::installs==0);
    }
    fixture::code_state=KILN_CODE_ORIGINAL;g_preview_failed=false;
    preview_hook_on_demand();CHECK(fixture::installs==2 && g_preview_hooked.load());
    uint8_t tx[0x100]={};void* job=reinterpret_cast<void*>(1);void* ctx=&job;wr(tx,0,ctx);wr(tx,8,reinterpret_cast<void*>(1));
    uint16_t accessor=1;wr(tx,0x18,&accessor);wr(tx,0x20,&accessor);tx[0xb1]=2;fixture::storage[8]=2;wr(fixture::storage,0,reinterpret_cast<void*>(1));
    wr(fixture::recipe,0x10,uint32_t{7});
    uint8_t lines[48]={};wr(lines,0x10,uint32_t{7});wr(lines,0x14,uint32_t{5});wr(lines,0x28,uint32_t{8});wr(lines,0x2c,uint32_t{2});
    struct {void* ptr;uint64_t n;} costs{lines,2};
    fixture::ret=g_preview_ret;
    auto run=[&] {return hk_preview_apply(tx,fixture::recipe,&costs,0,nullptr,nullptr,1,1);};
    uint32_t display=99;
    CHECK(craft_adjust_impl(7,10,4,nullptr,&display)==0); // no status-as-count shortcut, even with stock
    CHECK(run()==0 && fixture::payment_calls==1); // linked-only authorized stock, native material shortage only
    CHECK(!t_preview.tx); // scope never leaks to a real craft, another recipe or another worker
    CHECK(craft_preview_impl(tx,&costs,1,1,nullptr,g_preview_paid,4)==4);
    stock[1].count=1;t_set_craft_stock(stock,2);CHECK(run()==4); // every ingredient, not just the displayed recipe count
    stock[1].count=2;t_set_craft_stock(stock,2);
    fixture::eligibility=4;const auto called=fixture::payment_calls;CHECK(run()==4 && fixture::payment_calls==called);
    fixture::eligibility=5;CHECK(run()==5);fixture::eligibility=0;
    for(uint32_t code : {2u,3u,5u}) {fixture::native_payment=code;CHECK(run()==code);}
    fixture::native_payment=4;
    fixture::native_error=0x0b;CHECK(run()==4);fixture::native_error=0;
    fixture::observe_take=false;CHECK(run()==4);fixture::observe_take=true;
    fixture::null_take=true;CHECK(run()==4);fixture::null_take=false;
    fixture::bad_remainder=true;CHECK(run()==4);fixture::bad_remainder=false;
    for(uint32_t code : {2u,4u,5u}) {fixture::after_payment=code;CHECK(run()==code);} // room, water, output checks remain native
    fixture::after_payment=0;
    fixture::native_payment=0;CHECK(run()==0); // bag/native/CBS already pays: unmodified
    fixture::native_payment=4;
    fixture::ret=0x9999;CHECK(run()==4);fixture::ret=g_preview_ret; // actual hosted/server RecipeApply call is NOT the UI dry run
    fixture::storage[8]=0;CHECK(run()==4);fixture::storage[8]=2; // dedicated/hosted authoritative storage stays untouched
    tx[0xb0]=0x40;CHECK(run()==4);tx[0xb0]=0;
    tx[0xb1]=1;CHECK(run()==4);tx[0xb1]=2;
    E.tx_ok=false;CHECK(run()==4);E.tx_ok=true;
    E.item_find=nullptr;CHECK(run()==4);E.item_find=&item_find;
    wr(tx,8,uint64_t{0});CHECK(run()==4);wr(tx,8,reinterpret_cast<void*>(1));
    fixture::initialized=false;CHECK(run()==4);fixture::initialized=true;
    fixture::pages_ok=false;CHECK(run()==4);fixture::pages_ok=true;
    wr(fixture::recipe,0x10,uint32_t{99});CHECK(run()==4);wr(fixture::recipe,0x10,uint32_t{7});
    wr(lines,0x14,uint32_t{0});CHECK(run()==4);wr(lines,0x14,uint32_t{5});
    wr(lines,0x28,uint32_t{7});CHECK(run()==4);wr(lines,0x28,uint32_t{8}); // duplicate lines cannot spend one snapshot twice
    CHECK(hk_preview_apply(tx,fixture::recipe,&costs,0,nullptr,nullptr,1,2)==4); // multiplication must be covered too
    fixture::code_state=KILN_CODE_FOREIGN;CHECK(run()==4);fixture::code_state=KILN_CODE_MANAGED;CHECK(run()==0);
    t_set_craft_stock(nullptr,0);CHECK(run()==4); // missing/cleared snapshot is unknown, not free
    std::printf("craft preview: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
