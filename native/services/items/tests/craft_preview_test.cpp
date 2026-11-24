// SPDX-License-Identifier: MIT
// #132: the actual scoped client RecipeApply/InventoryPay preview bridge on fake game functions. No game is run.
#include "../stations.cpp"
#include <cstdarg>
#include <string>
#include <map>
namespace fixture {
uintptr_t ret = 0;
bool initialized=true, pages_ok=true, observe_take=true, null_take=false, bad_remainder=false;
bool skip_base_fetch=false, null_fetch=false, empty_fetch_view=false, unrelated_fetch=false;
uint32_t last_outer_error=99,last_outer_left=0,base_fetches=0;
uint8_t native_error=0, base_page_error=0;
using FetchFn = void*(__fastcall*)(void*,void*,uint32_t);
FetchFn fetch_detour=nullptr;
uintptr_t fetch_ret=0x14041154b;
std::map<uintptr_t,uint32_t> code_states;
uintptr_t late_foreign=0;
uint32_t installs=0, eligibility=0, after_payment=0, native_payment=4, payment_calls=0;
uint8_t storage[32]={}, recipe[0xb0]={};
std::string last_warn, last_info;
uintptr_t report_conflict_at=0;
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
void items_log(int level,const char* f,...) {
    if(level!=KILN_LOG_WARN && level!=KILN_LOG_INFO)return;
    char b[1024];va_list a;va_start(a,f);std::vsnprintf(b,sizeof b,f,a);va_end(a);
    (level==KILN_LOG_WARN ? fixture::last_warn : fixture::last_info)=b;
}
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
void* __fastcall fetch(void* out,void*,uint32_t page) {
    if(page==2100)++fixture::base_fetches;
    std::memset(out,0,56);wr(out,0,page==2100?fixture::base_page_error:page==2400?uint8_t{0x1f}:uint8_t{0});
    if(!rd<uint8_t>(out,0) && !fixture::empty_fetch_view)wr(out,8,reinterpret_cast<void*>(1));
    return fixture::null_fetch?nullptr:out;
}
PayOut* __fastcall take(PayOut* out,void* tx,uint32_t entity,uint32_t,uint32_t count) {
    uint8_t page[56]={};const auto get=fixture::fetch_detour?fixture::fetch_detour:&fetch;
    if(fixture::unrelated_fetch)get(page,reinterpret_cast<void*>(2),2400);
    if(!(fixture::skip_base_fetch && entity==2100))get(page,tx,entity==1?99:entity);
    // Native InventoryRemoveItems skips a failed page, yet returns error 0 and the unpaid remainder.
    *out={};out->error=fixture::native_error;out->left=count+fixture::bad_remainder;
    fixture::last_outer_error=out->error;fixture::last_outer_left=out->left;
    return fixture::null_take?nullptr:out;
}
uint32_t __fastcall apply(void* tx,const void*,const void* inputs,uint64_t,const void*,const uint32_t*,uint32_t owner,int32_t amount) {
    if(fixture::eligibility)return fixture::eligibility;
    ++fixture::payment_calls;
    PayOut out{};if(fixture::observe_take) {hk_preview_take(&out,tx,owner,7,5);hk_preview_take(&out,tx,2100,7,5);}
    if(fixture::late_foreign)fixture::code_states[fixture::late_foreign]=KILN_CODE_FOREIGN;
    const uint32_t paid=craft_preview_impl(tx,const_cast<void*>(inputs),owner,amount,nullptr,g_preview_paid,fixture::native_payment);
    return paid?paid:fixture::after_payment;
}
KilnResult KILN_CALL raw(KilnOwner,uintptr_t target,const KilnHookOrder*,void* detour,void** next,KilnHandle* h) {
    ++fixture::installs;*next=reinterpret_cast<void*>(target);*h=target;if(target==reinterpret_cast<uintptr_t>(&fetch))fixture::fetch_detour=reinterpret_cast<fixture::FetchFn>(detour);return KILN_OK;
}
KilnResult KILN_CALL code_state(uintptr_t at,size_t n,uint32_t* out,uint64_t* generation) {
    *out=KILN_CODE_ORIGINAL;*generation=1;
    for(auto [point,state]:fixture::code_states)if(point>=at && point-at<n) {*out=state;break;}
    return KILN_OK;
}
KilnResult KILN_CALL revoke(KilnHandle) {return KILN_OK;}
uintptr_t KILN_CALL caller(KilnHandle h) {return h==reinterpret_cast<uintptr_t>(&fetch)?fixture::fetch_ret:fixture::ret;}
KilnHooksApi hooks=[] {KilnHooksApi t{};t.header={sizeof t,KILN_HOOKS_VERSION};t.raw_detour=&raw;t.caller=&caller;t.revoke=&revoke;t.code_state=&code_state;return t;}();
KilnResult KILN_CALL describe_conflict(uintptr_t at,size_t n,char* text,size_t size) {
    if(fixture::report_conflict_at < at || fixture::report_conflict_at-at >= n) return KILN_ERR_NOT_FOUND;
    std::snprintf(text,size," (first unrecognised difference at +0x%llx, call redirected to example.dll)",
                  static_cast<unsigned long long>(fixture::report_conflict_at-at));
    return KILN_OK;
}
KilnHooksInternalApi internal_hooks={{sizeof internal_hooks,KILN_HOOKS_INTERNAL_VERSION},nullptr,&describe_conflict};
const void* KILN_CALL service(KilnStr name,uint32_t) {
    if(std::string_view(name.ptr,name.len)==KILN_HOOKS_INTERNAL_SERVICE) return &internal_hooks;
    return &hooks;
}
}
int main() {
    using namespace kitems;
    int checks=0,failed=0;
#define CHECK(x) do {++checks;if(!(x)){++failed;std::printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
    KilnApi api{};api.size=sizeof api;api.service=&service;g_api=&api;g_owner=10;
    E.tx_ok=true;E.job_storage=&job_storage;E.component_map_find=&component_index;E.read_other=&read_other;
    G.resolve=&resolve;G.recipe_find=&recipe_find;E.item_find=&item_find;g_preview_paid=0x1234;g_client_recipes.store(reinterpret_cast<void*>(1));
    g_preview_apply=reinterpret_cast<uintptr_t>(&apply);g_preview_ret=0x5678;g_preview_pay=0x9876;g_preview_take=reinterpret_cast<uintptr_t>(&take);g_preview_feat=true;
    g_preview_remove=0x140411360;g_preview_stock=0x14046fa10;g_preview_rollback=0x140418150;
    g_preview_fetch=reinterpret_cast<uintptr_t>(&fetch);g_preview_fetch_ret=0x14041154b;g_preview_preferred_ret=0x140411474;
    KilnStationStack stock[]={{7,5},{8,2}};CHECK(t_set_craft_stock(stock,2)==KILN_OK);
    // A foreign hook (including CBS at this site) or unknown code means no second patch and no attempt to enable.
    for(uint32_t state : {KILN_CODE_FOREIGN,KILN_CODE_UNKNOWN}) {
        fixture::code_states[g_preview_apply]=state;g_preview_failed=false;preview_hook_on_demand();CHECK(fixture::installs==0);
    }
    // The stand-down line names the occupied dependency, e.g. CBS changing BaseStockForPlayer or the material taker.
    for(auto [at,name] : {std::pair<uintptr_t,const char*>{0x14046fa70,"BaseStockForPlayer"},{0x140411380,"InventoryRemoveItems"},
                          {0x1404185a0,"InventoryRollback"}}) {
        fixture::code_states.clear();fixture::code_states[at]=KILN_CODE_FOREIGN;fixture::last_warn.clear();g_preview_failed=false;
        preview_hook_on_demand();CHECK(fixture::installs==0 && fixture::last_warn.find(name)!=std::string::npos);
    }
    fixture::code_states.clear();g_preview_failed=false;
    fixture::report_conflict_at=g_preview_pay+0x10e;
    fixture::code_states[fixture::report_conflict_at]=KILN_CODE_FOREIGN;
    preview_hook_on_demand();
    CHECK(fixture::installs==0 && fixture::last_warn.find("InventoryPay code state FOREIGN")!=std::string::npos &&
          fixture::last_warn.find("+0x10e, call redirected to example.dll")!=std::string::npos);
    fixture::report_conflict_at=0;fixture::code_states.clear();g_preview_failed=false;
    preview_hook_on_demand();CHECK(fixture::installs==3 && g_preview_hooked.load());
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
    CHECK(!t_preview.tx && !t_preview_diag); // scope never leaks to a real craft, another recipe or another worker
    CHECK(craft_preview_impl(tx,&costs,1,1,nullptr,g_preview_paid,4)==4);
    stock[1].count=1;t_set_craft_stock(stock,2);CHECK(run()==4); // every ingredient, not just the displayed recipe count
    CHECK(fixture::last_info.find("snapshot-insufficient (payment 4, item 0x00000008, need 2, counted 1")!=std::string::npos);
    stock[1].count=2;t_set_craft_stock(stock,2);
    fixture::eligibility=4;const auto called=fixture::payment_calls;CHECK(run()==4 && fixture::payment_calls==called);
    CHECK(fixture::last_info.find("no-matching-payment (payment not-observed")!=std::string::npos);
    fixture::eligibility=5;CHECK(run()==5);fixture::eligibility=0;
    for(uint32_t code : {2u,3u,5u}) {fixture::native_payment=code;CHECK(run()==code);}
    fixture::native_payment=4;
    fixture::base_page_error=0x1f;fixture::base_fetches=0;CHECK(run()==4);
    CHECK(fixture::last_info.find("native-page-read-error")!=std::string::npos);
    CHECK(fixture::base_fetches==1 && fixture::last_outer_error==0 && fixture::last_outer_left==5);
    fixture::native_payment=0;CHECK(run()==0);fixture::native_payment=4;fixture::base_page_error=0;
    fixture::skip_base_fetch=true;CHECK(run()==4);fixture::skip_base_fetch=false;
    fixture::null_fetch=true;CHECK(run()==4);fixture::null_fetch=false;
    fixture::empty_fetch_view=true;CHECK(run()==4);fixture::empty_fetch_view=false;
    fixture::fetch_ret=0x9999;CHECK(run()==4);fixture::fetch_ret=g_preview_preferred_ret;CHECK(run()==0);fixture::fetch_ret=g_preview_fetch_ret;
    fixture::unrelated_fetch=true;CHECK(run()==0);fixture::unrelated_fetch=false;
    uint8_t outside[56]={};CHECK(hk_preview_fetch(outside,tx,2400)==outside && outside[0]==0x1f);CHECK(run()==0);
    CHECK(!t_preview_fetch);
    fixture::native_error=0x0b;CHECK(run()==4);fixture::native_error=0;
    g_log_preview_payment.budget.store(2);
    fixture::observe_take=false;CHECK(run()==4);fixture::observe_take=true;
    CHECK(fixture::last_info.find("no-native-material-take")!=std::string::npos);
    fixture::null_take=true;CHECK(run()==4);fixture::null_take=false;
    fixture::bad_remainder=true;CHECK(run()==4);fixture::bad_remainder=false;
    for(uint32_t code : {2u,4u,5u}) {fixture::after_payment=code;CHECK(run()==code);} // room, water, output checks remain native
    fixture::after_payment=0;
    fixture::native_payment=0;CHECK(run()==0); // bag/native/CBS already pays: unmodified
    fixture::native_payment=4;
    fixture::last_info.clear();fixture::ret=0x9999;CHECK(run()==4);fixture::ret=g_preview_ret;
    CHECK(fixture::last_info.empty()); // hosted/server RecipeApply is not this desktop's exact UI dry run
    fixture::storage[8]=0;CHECK(run()==4);fixture::storage[8]=2;
    CHECK(fixture::last_info.find("client-storage")!=std::string::npos);
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
    fixture::code_states[g_preview_apply]=KILN_CODE_FOREIGN;CHECK(run()==4);
    fixture::code_states[g_preview_apply]=KILN_CODE_MANAGED;CHECK(run()==0);fixture::code_states.clear();
    // Address-specific dependency checks, including changes after entering the preview's original call.
    for(uintptr_t at : {g_preview_apply+0x10,g_preview_pay+0x10,g_preview_take+0x10,g_preview_ret,
                          uintptr_t{0x140411380},uintptr_t{0x14046fa70},uintptr_t{0x1404185a0},reinterpret_cast<uintptr_t>(&fetch)+0x20}) {
        for(uint32_t state : {KILN_CODE_FOREIGN,KILN_CODE_UNKNOWN}) {
            fixture::code_states[at]=state;CHECK(run()==4);
            fixture::native_payment=0;CHECK(run()==0);fixture::native_payment=4;fixture::code_states.clear();
        }
        fixture::code_states[at]=KILN_CODE_MANAGED;CHECK(run()==0);fixture::code_states.clear();
        fixture::late_foreign=at;CHECK(run()==4);fixture::late_foreign=0;fixture::code_states.clear();
        h_preview_apply={};h_preview_take={};g_preview_hooked.store(false);g_preview_failed=false;fixture::installs=0;
        for(uint32_t state : {KILN_CODE_FOREIGN,KILN_CODE_UNKNOWN}) {
            fixture::code_states[at]=state;g_preview_failed=false;preview_hook_on_demand();CHECK(fixture::installs==0);
        }
        fixture::code_states.clear();g_preview_failed=false;preview_hook_on_demand();
    }
    t_set_craft_stock(nullptr,0);CHECK(run()==4); // missing/cleared snapshot is unknown, not free
    std::printf("craft preview: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
