// SPDX-License-Identifier: MIT
// Executes the production entry/payment callbacks and client mids against a Windows fake inventory transaction.
#include <cstdio>
#include <map>
#include "../dyes.cpp"
namespace {
int checks=0,failures=0,bag=0,chest=1,palette=0,calls=0,takes=0;
bool allowed=true,registered=true,generation=true,deny=false,revoke_in_handler=false,remove_in_handler=false,seh_fault=false,commit_guard=false;
bool inspect_ordinary=false,wrong_original=false,fail_entry=false;
uint32_t live_code=KILN_CODE_ORIGINAL;
int installs=0,code_reads=0,state_reads=0;
std::map<uintptr_t,std::vector<uint8_t>> originals;
KilnResult KILN_CALL original_code(uintptr_t at,void* out,size_t n){
 ++code_reads;auto it=originals.find(at);if(it==originals.end()||it->second.size()<n)return KILN_ERR_UNAVAILABLE;
 std::memcpy(out,it->second.data(),n);if(wrong_original)static_cast<uint8_t*>(out)[0]^=1;return KILN_OK;
}
KilnResult KILN_CALL code_state(uintptr_t,size_t,uint32_t* state,uint64_t* epoch){++state_reads;*state=live_code;*epoch=1;return KILN_OK;}
KilnResult KILN_CALL symbol(KilnStr name,uintptr_t* out){
 const std::string s(name.ptr,name.len);*out=s=="dyes/TintAction"?0x1000:s=="items/InventoryRemoveItems"?0x2000:0;return *out?KILN_OK:KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL tick_sub(KilnOwner,uint32_t,int32_t,KilnTickFn,void*,KilnHandle*){return KILN_OK;}
KilnResult KILN_CALL world_sub(KilnOwner,KilnWorldEventFn,void*,KilnHandle*){return KILN_OK;}
uint8_t registry[256]{},storage[64]{},info[0x800]{};
void* returned_storage=storage;
uint16_t types[kitems::A_COUNT]{1,2,3,4,5,6,7};
void* job=reinterpret_cast<void*>(1);uint64_t ctx[2]{reinterpret_cast<uint64_t>(&job),1};
uint64_t iterator[64]{};
KilnApi fake_api{};KilnHooksApi fake_hooks{};KilnCapsApi fake_caps{},other_caps{};
KilnCapsApi* caps_provider=nullptr;KilnOwner owner=1;
uintptr_t server_world=100,local_world=200;uint64_t server_session=1,local_session=1;
int cap_lookups=0,cap_reports=0;bool fail_report=false;
struct CapRow {uint32_t state=0,reason=0;std::string text;};
std::map<std::string,CapRow> cap_rows;
KilnResult KILN_CALL cap_report(KilnOwner,KilnStr name,uint32_t state,uint32_t reason,KilnStr text){
 ++cap_reports;if(fail_report){fail_report=false;return KILN_ERR_UNAVAILABLE;}
 cap_rows[std::string(name.ptr,name.len)]={state,reason,std::string(text.ptr,text.len)};return KILN_OK;
}
uintptr_t KILN_CALL world(uint32_t kind){return kind==KILN_WORLD_SERVER?server_world:local_world;}
KilnResult KILN_CALL session(KilnSessionInfo* s){
 s->sessions[KILN_WORLD_SERVER]=server_session;s->sessions[KILN_WORLD_CLIENT]=local_session;return KILN_OK;
}
void check(bool b,const char* s){++checks;std::printf("%s %s\n",b?"PASS":"FAIL",s);failures+=!b;}
uint32_t KILN_CALL side(){return KILN_SIDE_CLIENT;}
const void* KILN_CALL service(KilnStr name,uint32_t){
 const std::string s(name.ptr,name.len);if(s==KILN_CAPS_SERVICE){++cap_lookups;return caps_provider;}
 return s==KILN_HOOKS_SERVICE?&fake_hooks:nullptr;
}
KilnResult KILN_CALL raw(KilnOwner,uintptr_t target,const KilnHookOrder*,void*,void** next,KilnHandle* h){++installs;if(fail_entry&&target==0x1000)return KILN_ERR_UNAVAILABLE;*next=reinterpret_cast<void*>(target);*h=1;return KILN_OK;}
KilnResult KILN_CALL revoke(KilnHandle){return KILN_OK;}
uintptr_t KILN_CALL caller(KilnHandle){return kitems::pay_ret;}
void* __fastcall job_storage(void*){return returned_storage;}
uint16_t* __fastcall component(void*,const uint32_t* hash){for(int i=0;i<kitems::A_COUNT;++i)if(*hash==kitems::kAccHashes[i])return &types[i];return nullptr;}
void* __fastcall item_find(uint32_t item,void*){return item==123?info:nullptr;}
uint32_t* __fastcall actor(void*,void* out){*static_cast<uint32_t*>(out)=1;return static_cast<uint32_t*>(out);}
kitems::Result12* __fastcall remove_one(kitems::Result12* out,void*,uint32_t holder,uint32_t,uint64_t,uint32_t){
 ++takes;int& n=holder==1?bag:chest;
 if(holder!=1&&seh_fault){if(n)--n;RaiseException(0xe0000001,0,0,nullptr);}
 if(n){--n;*out={};}else *out={0,{0,0,0},0,1};return out;
}
void __fastcall vanilla(const uint64_t* action,const uint64_t*){
 ++calls;if(inspect_ordinary){check(kitems::route==nullptr,"an ordinary nested Tint cannot inherit a reserved-key payment route");return;}
 check(KILN_SLOT_INDEX(action[1])<8,"server entry resolves the reserved key to a live ordinary slot");
 alignas(16) uint8_t tx[0x71b0]{};
 kitems::dyes::put(tx,0,reinterpret_cast<uint64_t>(ctx));kitems::dyes::put(tx,8,uint64_t(1));
 for(const auto& pair:std::initializer_list<std::pair<int,int>>{{4,kitems::A_INVENTORY},{0x14,kitems::A_EMPTY},{3,kitems::A_SETUP},{5,kitems::A_ITEMSTATE},{0x12,kitems::A_OWNER},{0x1d,kitems::A_SLOTSEL}})
  kitems::dyes::put(tx,pair.first*8,reinterpret_cast<uint64_t>(&types[pair.second]));
 kitems::dyes::put(tx,0x70,uint64_t(1));if(commit_guard)tx[0xb0]=0x40;
 if(revoke_in_handler)allowed=false;if(remove_in_handler)registered=false;
 int before_bag=bag,before_chest=chest;
 kitems::Result12 out{};kitems::hk_pay(&out,tx,1,123,action[1],1);
 if(out.error||out.value){bag=before_bag;chest=before_chest;}else palette=1;
}
std::array<kitems::dyes::View,16> ui_rows;
int drawn=0;
void __fastcall build_palette(void* h){
 ui_rows={};ui_rows[0]=kitems::dyes::view(999,1);
 kitems::dyes::put(h,0,reinterpret_cast<uint64_t>(ui_rows.data()));kitems::dyes::put(h,8,uint64_t(1));kitems::dyes::put(h,16,uint64_t(16));
}
void __fastcall draw_palette(void* state){
 auto* h=static_cast<uint8_t*>(state)+0x90b80;
 auto* rows=reinterpret_cast<kitems::dyes::View*>(kitems::dyes::get<uintptr_t>(h,0));
 drawn=static_cast<int>(kitems::dyes::get<uint64_t>(h,8));
 for(int i=0;i<drawn;++i){KilnRegisters r{};r.rbx=reinterpret_cast<uintptr_t>(rows[i].data());r.rax=0;kitems::on_row(r);kitems::dyes::put(rows[i].data(),16,static_cast<uint32_t>(r.rax));}
}
void reset(){bag=0;chest=1;palette=calls=takes=0;allowed=registered=generation=true;deny=revoke_in_handler=remove_in_handler=seh_fault=commit_guard=false;
 kitems::faulted.store(false);kitems::server_ok.store(true);kitems::active_server_storage.store(storage);storage[8]=0;returned_storage=storage;kitems::route=nullptr;}
}
namespace kitems {
Engine E;
const uint32_t kAccHashes[A_COUNT]{1,2,3,4,5,6,7};
const char* const kAccNames[A_COUNT]{"Fuel","Inventory","InventoryEmptyState","InventorySetup","ItemState","OwnerRelationship","SlotSelection"};
const KilnApi* items_api(){return &fake_api;}
KilnOwner items_owner(){return owner;}
std::vector<CraftSource> craft_sources_copy(uint32_t player){if(player!=1||!registered||!generation)return {};CraftSource s{};s.player=1;s.container=2000;s.deny=!deny;s.items=deny?std::vector<uint32_t>{}:std::vector<uint32_t>{};return {s};}
bool craft_source_access(void*,uint32_t player,uint32_t container){return allowed&&player==1&&container==2000;}
bool craft_source_access(uint32_t player,uint32_t container){return craft_source_access(nullptr,player,container);}
bool read_pages(const Access&,KilnEntity holder,PageSet* out,bool* initialized){*out={};*initialized=true;out->n=1;out->p[0].entity=holder==1?1000:2000;
 out->p[0].s[0]={KILN_SLOT_ID(out->p[0].entity,0),123,static_cast<uint32_t>(holder==1?bag:chest),0};return true;}
}
int main(){
 using namespace kitems;
 fake_api.size=sizeof fake_api;fake_api.service=&service;fake_api.side=&side;
 fake_hooks.header={sizeof fake_hooks,2};fake_hooks.raw_detour=&raw;fake_hooks.caller=&caller;fake_hooks.revoke=&revoke;
 api=&fake_api;actor_fn=&actor;pay_ret=0x555;
 E.job_storage=&job_storage;E.component_map_find=&component;E.item_find=&item_find;
 dyes::put(storage,0,reinterpret_cast<uint64_t>(registry));info[0xcc]=16;info[0x748]=1;
 iterator[0]=reinterpret_cast<uint64_t>(ctx);iterator[0xd8/8]=1;iterator[0xc8/8]=1;
 for(const auto& p:std::initializer_list<std::pair<int,int>>{{0x148,A_INVENTORY},{0x158,A_EMPTY},{0x160,A_SETUP},{0x168,A_ITEMSTATE},{0x178,A_OWNER},{0x190,A_SLOTSEL}})iterator[p.first/8]=reinterpret_cast<uint64_t>(&types[p.second]);
 check(entry_hook.install(&fake_api,1,reinterpret_cast<uintptr_t>(&vanilla),reinterpret_cast<void*>(&hk_entry),"entry"),"entry hook continuation installs");
 check(pay_hook.install(&fake_api,1,reinterpret_cast<uintptr_t>(&remove_one),reinterpret_cast<void*>(&hk_pay),"payment"),"payment hook continuation installs");
 uint64_t action[2]{KILN_SLOT_ID(1001,0),dyes::token(123)};
 reset();hk_entry(action,iterator);check(palette==1&&chest==0,"source-only dye pays within the original Tint transaction");check(route==nullptr,"entry restores thread-local routing");
 reset();bag=1;hk_entry(action,iterator);check(palette==1&&bag==0&&chest==1&&takes==1,"bag pays first when a registered source also holds dye");
 reset();revoke_in_handler=true;hk_entry(action,iterator);check(palette==0&&chest==1,"permission revoked between entry and payment refuses without colouring");
 reset();remove_in_handler=true;hk_entry(action,iterator);check(palette==0&&chest==1,"registration removed between entry and payment refuses");
 reset();generation=false;hk_entry(action,iterator);check(calls==0&&palette==0,"a reused player session cannot borrow old registrations");
 reset();deny=true;hk_entry(action,iterator);check(calls==0,"item filters apply to entry resolution");
 reset();storage[8]=2;hk_entry(action,iterator);check(calls==0,"a client-world accessor is refused");
 reset();uint8_t old_storage[64]{};std::memcpy(old_storage,storage,sizeof storage);returned_storage=old_storage;hk_entry(action,iterator);
 check(calls==0&&palette==0,"a different server world's storage cannot use the active world's registrations");
 reset();iterator[0x148/8]=0;hk_entry(action,iterator);check(calls==0,"missing source Inventory accessor is refused");iterator[0x148/8]=reinterpret_cast<uint64_t>(&types[A_INVENTORY]);
 reset();commit_guard=true;hk_entry(action,iterator);check(palette==0&&chest==1&&takes==0,"a commit that would roll back is refused before the palette write");
 reset();seh_fault=true;hk_entry(action,iterator);check(palette==0&&chest==1&&faulted.load(),"an SEH fault after a source debit rolls back before tinting");
 reset();server_ok.store(false);hk_entry(action,iterator);check(calls==0&&chest==1,"without an available Kiln server the reserved key never reaches vanilla");
 reset();Route outer{1,123,ctx,storage};route=&outer;inspect_ordinary=true;
 uint64_t ordinary[2]{action[0],KILN_SLOT_ID(1000,0)};hk_entry(ordinary,iterator);
 check(route==&outer,"a nested ordinary Tint restores its caller's payment route");inspect_ordinary=false;route=nullptr;
 std::vector<dyes::Stock> stock{{123,3}};auto row=dyes::view(123,0);KilnRegisters r{};r.rax=5;r.rbx=reinterpret_cast<uintptr_t>(row.data());r.rflags=0x246;drawing=&stock;on_row(r);
 check(r.rax==8&&r.rflags==0x246,"palette row mid updates only the count register");r.rax=7;r.r15=123;on_detail(r);check(r.rax==10,"palette detail mid adds registered dye counts");
 drawing=nullptr;KilnRegisters before=r;on_row(r);on_detail(r);check(std::memcmp(&r,&before,sizeof r)==0,"client mids leave every register alone without a live supported offer");
 ui_feature=true;client_world.store(1);KilnStationStack rows[2]{{123,2},{123,3}};
 check(dyes_set_stock(1,rows,2)==KILN_ERR_INVALID_ARGUMENT,"duplicate snapshot items are refused");
 check(dyes_set_stock(1,rows,1)==KILN_OK&&!current_offer(1).empty(),"a live server announcement enables only its player");
 check(current_offer(2).empty(),"a different local player cannot see another player's offer");
 client_epoch.fetch_add(1);check(current_offer(1).empty(),"a different client world cannot inherit an old support announcement");
 check(dyes_set_stock(1,rows,1)==KILN_OK,"the current world may receive a fresh server announcement");
 check(dyes_set_stock(0,nullptr,0)==KILN_OK&&current_offer(1).empty(),"clearing the server announcement removes all source rows");
 std::vector<uint8_t> play(0x1ce0),state(0x90ba0),ui_app(0xd8),ui_data(8),app_state(0x40);
 uintptr_t app_cell=reinterpret_cast<uintptr_t>(ui_app.data());app_at=reinterpret_cast<uintptr_t>(&app_cell);
 dyes::put(ui_app.data(),0xd0,reinterpret_cast<uintptr_t>(ui_data.data()));dyes::put(ui_data.data(),0,reinterpret_cast<uintptr_t>(app_state.data()));
 dyes::put(app_state.data(),0xc,uint8_t(KILN_UI_STATE_PLAY));dyes::put(app_state.data(),0x38,reinterpret_cast<uintptr_t>(play.data()));
 dyes::put(play.data(),0x18,uint32_t(1));dyes::put(play.data(),0x1cd8,uint8_t(42));
 build_fn=&build_palette;build_palette(state.data()+0x90b80);ui_ok.store(true);
 check(grid_hook.install(&fake_api,1,reinterpret_cast<uintptr_t>(&draw_palette),reinterpret_cast<void*>(&hk_grid),"palette"),"palette hook continuation installs");
 hk_grid(state.data());check(drawn==1,"without a supported server announcement the palette hook adds no row");
 dyes_set_stock(1,rows,1);hk_grid(state.data());check(drawn==2&&dyes::id(ui_rows[0])==123&&dyes::key(ui_rows[0])==dyes::token(123)&&dyes::count(ui_rows[0])==2&&dyes::id(ui_rows[1])==999,"palette hook injects a reserved-key source dye, sorts it with the dyes the player has by count (#132), and the live row mid supplies its count");
 dyes_set_stock(0,nullptr,0);hk_grid(state.data());check(drawn==1,"closing or losing the server offer rebuilds the unmodified game palette list");
 std::vector<dyes::Stock> outer_drawing{{999,3}};drawing=&outer_drawing;ui_ok.store(false);modified_state=nullptr;
 hk_grid(state.data());check(dyes::count(ui_rows[0])==0,"an ordinary nested grid cannot inherit an outer source-count offer");
 check(drawing==&outer_drawing,"an ordinary nested grid restores its caller's drawing scope");drawing=nullptr;ui_ok.store(true);
 struct WireSchema {uintptr_t name=0,unused=0,fields=0;uint64_t count=1;};
 std::array<WireSchema,6> schemas{};std::array<std::array<uint8_t,64>,6> fields{};
 const char* names[]{"newestEntry","data","versionedActions","itemAction","secondarySlotId","slotIndex"};
 for(size_t i=0;i<6;++i){schemas[i].fields=reinterpret_cast<uintptr_t>(fields[i].data());dyes::put(fields[i].data(),0,reinterpret_cast<uintptr_t>(names[i]));dyes::put(fields[i].data(),8,uint32_t(i==5?4:12));dyes::put(fields[i].data(),16,uint32_t(1));dyes::put(fields[i].data(),40,uint64_t(1));if(i<5)dyes::put(fields[i].data(),56,reinterpret_cast<uintptr_t>(&schemas[i+1]));}
 dyes::put(fields[5].data(),24,uint64_t(4));uintptr_t root=reinterpret_cast<uintptr_t>(&schemas[0]);wire_at=reinterpret_cast<uintptr_t>(&root);
 check(wire_valid(),"full-u8 secondary slot wire schema qualifies");dyes::put(fields[5].data(),8,uint32_t(13));dyes::put(fields[5].data(),48,uint64_t(3));
 check(!wire_valid(),"a narrowed three-bit slot wire schema fails closed");
 fake_api.symbol=&symbol;fake_api.subscribe_world_tick=&tick_sub;fake_api.subscribe_world=&world_sub;
 originals[0x1000]={0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55,0x41,0x56,0x41,0x57};
 originals[0x2000]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20};
 server_feature=true;server_ok.store(false);installs=0;dyes_register();
 check(!server_ok.load()&&installs==0,"missing original-code APIs refuse both payment and entry hooks");
 fake_hooks.read_original=&original_code;fake_hooks.code_state=&code_state;
 live_code=KILN_CODE_FOREIGN;server_ok.store(false);installs=0;dyes_register();
 check(!server_ok.load()&&installs==0,"foreign live tint code refuses both hooks before installation");
 live_code=KILN_CODE_UNKNOWN;server_ok.store(false);installs=0;dyes_register();
 check(!server_ok.load()&&installs==0,"unreadable live tint code refuses both hooks");
 live_code=KILN_CODE_ORIGINAL;wrong_original=true;server_ok.store(false);installs=0;dyes_register();
 check(!server_ok.load()&&installs==0,"mismatched file prologues refuse both hooks");
 wrong_original=false;live_code=KILN_CODE_MANAGED;server_ok.store(false);installs=code_reads=state_reads=0;dyes_register();
 check(server_ok.load()&&installs==2&&code_reads>=2&&state_reads>=2,"file-qualified registry-managed code permits both hooks");
 entry_hook.revoke();pay_hook.revoke();fail_entry=true;server_ok.store(false);dyes_register();
 check(!server_ok.load()&&!entry_hook&&!pay_hook,"entry installation failure revokes the unused payment link");fail_entry=false;
 grid_at=0x3000;build_fn=reinterpret_cast<BuildFn>(0x4000);row_at=0x5000;detail_at=0x6000;
 originals[grid_at]={0x40,0x55,0x56,0x57,0x41,0x56,0x48,0x8d,0xac,0x24,0x18,0xf7,0xff,0xff};
 originals[0x4000]={0x40,0x55,0x53,0x57,0x41,0x54,0x41,0x56,0x48,0x8d,0x6c,0x24,0xc9};
 originals[row_at]={0x89,0x43,0x10};originals[detail_at]={0xc7,0x45,0xe8,0x01,0x00,0x00,0x00};
 check(client_code(),"file-qualified client grid builder and both count seams permit palette hooks");
 live_code=KILN_CODE_FOREIGN;check(!client_code(),"foreign client palette code is refused");live_code=KILN_CODE_ORIGINAL;
 originals[row_at][0]^=1;check(!client_code(),"a changed client count instruction is refused");
 fake_api.world=&world;fake_api.session=&session;fake_caps.header={sizeof fake_caps,1};fake_caps.report=&cap_report;
 other_caps=fake_caps;caps_provider=&fake_caps;faulted.store(false);server_ok.store(true);ui_ok.store(true);offer={};
 server_pre(nullptr,nullptr);client_pre(nullptr,nullptr);
 check(cap_reports==2&&cap_rows["crafting.dye_sources_session"].state==KILN_CAP_AVAILABLE&&cap_rows["crafting.dye_ui_session"].state==KILN_CAP_PENDING,"initial dye ticks report both session capabilities");
 int reports=cap_reports,lookups=cap_lookups;server_pre(nullptr,nullptr);client_pre(nullptr,nullptr);
 check(cap_reports==reports&&cap_lookups==lookups+2,"unchanged dye ticks report nothing and look up caps once per tick");
 server_ok.store(false);server_pre(nullptr,nullptr);
 check(cap_reports==reports+1&&cap_rows["crafting.dye_sources_session"].state==KILN_CAP_PENDING,"server hook uninstall changes its session capability");
 reports=cap_reports;server_ok.store(true);server_pre(nullptr,nullptr);
 check(cap_reports==reports+1&&cap_rows["crafting.dye_sources_session"].state==KILN_CAP_AVAILABLE,"server hook install restores its session capability");
 reports=cap_reports;++server_session;server_pre(nullptr,nullptr);
 check(cap_reports==reports+1,"replacement server session re-reports even with the same world and state");
 reports=cap_reports;server_pre(nullptr,nullptr);check(cap_reports==reports,"replacement server session settles without repeated reports");
 reports=cap_reports;++server_world;server_pre(nullptr,nullptr);check(cap_reports==reports+1,"replacement server world re-reports even with the same session id");
 reports=cap_reports;server_world=0;server_pre(nullptr,nullptr);
 check(cap_reports==reports+1&&cap_rows["crafting.dye_sources_session"].state==KILN_CAP_PENDING,"server world stopping makes its session capability pending");
 server_world=100;server_pre(nullptr,nullptr);reports=cap_reports;faulted.store(true);lookups=cap_lookups;server_pre(nullptr,nullptr);
 check(cap_reports==reports+2&&cap_lookups==lookups+1&&cap_rows["crafting.dye_sources"].reason==KILN_CAP_REASON_FAULTED,"faulted server reports the fault and session change with one caps lookup");
 reports=cap_reports;server_pre(nullptr,nullptr);check(cap_reports==reports,"a server payment fault is reported once, not every tick");
 faulted.store(false);server_ok.store(true);server_pre(nullptr,nullptr);reports=cap_reports;
 server_ok.store(false);fail_report=true;server_pre(nullptr,nullptr);
 check(cap_reports==reports+1&&cap_rows["crafting.dye_sources_session"].state==KILN_CAP_AVAILABLE,"a rejected session report is not delivered");
 reports=cap_reports;server_pre(nullptr,nullptr);
 check(cap_reports==reports+1&&cap_rows["crafting.dye_sources_session"].state==KILN_CAP_PENDING,"a rejected session report is retried next tick");
 reports=cap_reports;server_pre(nullptr,nullptr);check(cap_reports==reports,"a successful retry stops further session reports");
 ++owner;server_pre(nullptr,nullptr); // A new owner has not received the earlier fault.
 fail_report=true;faulted.store(true);server_pre(nullptr,nullptr);reports=cap_reports;server_pre(nullptr,nullptr);
 check(cap_reports==reports+1&&cap_rows["crafting.dye_sources"].reason==KILN_CAP_REASON_FAULTED,"a rejected fault report is retried once successfully");
 reports=cap_reports;server_pre(nullptr,nullptr);check(cap_reports==reports,"a delivered fault retry is not repeated");
 check(dyes_set_stock(1,rows,1)==KILN_OK,"capability test publishes a live client offer");
 reports=cap_reports;client_pre(nullptr,nullptr);
 check(cap_reports==reports+1&&cap_rows["crafting.dye_ui_session"].state==KILN_CAP_AVAILABLE,"a live offer and installed UI hooks make the client session available");
 reports=cap_reports;client_pre(nullptr,nullptr);check(cap_reports==reports,"unchanged supported client session reports nothing");
 reports=cap_reports;ui_ok.store(false);client_pre(nullptr,nullptr);
 check(cap_reports==reports+1&&cap_rows["crafting.dye_ui_session"].state==KILN_CAP_PENDING,"client hook uninstall updates its session capability");
 reports=cap_reports;ui_ok.store(true);client_pre(nullptr,nullptr);
 check(cap_reports==reports+1&&cap_rows["crafting.dye_ui_session"].state==KILN_CAP_AVAILABLE,"client hook install restores its session capability");
 reports=cap_reports;++local_session;client_pre(nullptr,nullptr);
 check(cap_reports==reports+1,"replacement client session re-reports even with the same world and state");
 reports=cap_reports;++local_world;client_pre(nullptr,nullptr);
 check(cap_reports==reports+1&&cap_rows["crafting.dye_ui_session"].state==KILN_CAP_PENDING,"replacement client world clears the offer and reports pending");
 reports=cap_reports;client_pre(nullptr,nullptr);check(cap_reports==reports,"replacement client world settles without repeated reports");
 reports=cap_reports;caps_provider=&other_caps;server_pre(nullptr,nullptr);client_pre(nullptr,nullptr);
 check(cap_reports==reports+3,"replacement caps provider receives both session rows and the fault");
 reports=cap_reports;++owner;server_pre(nullptr,nullptr);client_pre(nullptr,nullptr);
 check(cap_reports==reports+3,"replacement report owner receives both session rows and the fault");
 caps_provider=nullptr;server_pre(nullptr,nullptr);client_pre(nullptr,nullptr);caps_provider=&other_caps;reports=cap_reports;
 server_pre(nullptr,nullptr);client_pre(nullptr,nullptr);check(cap_reports==reports+3,"caps returning after an outage receives all current dye reports");
 KilnWorldEvent stopped{};stopped.event=KILN_WORLD_STOPPING;stopped.kind=KILN_WORLD_SERVER;world_event(&stopped,nullptr);reports=cap_reports;
 world_event(&stopped,nullptr);check(cap_reports==reports,"duplicate server stop events do not repeat an identical capability report");
 stopped.kind=KILN_WORLD_CLIENT;world_event(&stopped,nullptr);reports=cap_reports;world_event(&stopped,nullptr);
 check(cap_reports==reports,"duplicate client stop events do not repeat an identical capability report");
 std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
