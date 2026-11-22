// SPDX-License-Identifier: MIT
// Registered crafting sources also supply dyes (#125). A source row sends an item id, never a chest slot. The
// reserved secondary slot index 0xff survives the full-u8 input wire; without this entry hook SlotInfo refuses it.
// The entry only resolves a live slot. The payment rechecks the registration, player generation and access inside
// Tint's own transaction, before either ColorPaletteItem write. Every payment failure reaches Tint's own rollback.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <mutex>
#include <vector>
#include "dyes.h"
#include "dye_sources_logic.h"
#include "engine.h"
#include "xfer.h"
#include "stations.h"
#include "craft_sources.h"
#include "kiln_hook.hpp"
#include "services/kiln_caps.h"
#include "services/kiln_ui.h"
namespace kitems {
namespace {
using namespace dyes;
using EntryFn=void(__fastcall*)(const uint64_t*,const uint64_t*);
using ActorFn=uint32_t*(__fastcall*)(void*,void*);
using BuildFn=void(__fastcall*)(void*);
using GridFn=void(__fastcall*)(void*);
using GrowFn=uint8_t(__fastcall*)(void*,int64_t,uint8_t);
const KilnApi* api=nullptr;
kiln::RawHook entry_hook,pay_hook,grid_hook;
kiln::MidHook2 row_hook,detail_hook;
ActorFn actor_fn=nullptr;
BuildFn build_fn=nullptr;
uintptr_t pay_ret=0,grid_at=0,row_at=0,detail_at=0,app_at=0,wire_at=0;
std::atomic<bool> server_ok{false},ui_ok{false},faulted{false};
bool server_feature=false,ui_feature=false;
std::atomic<void*> active_server_storage{nullptr};
std::atomic<uintptr_t> client_world{0};
std::atomic<uint64_t> client_epoch{0};
struct Offer {uint32_t player=0;uint64_t epoch=0,expires=0;std::vector<Stock> stock;};
std::mutex offer_mu;
Offer offer;
struct Route {uint32_t player=0,item=0;void* ctx=nullptr;void* storage=nullptr;};
thread_local Route* route=nullptr;
thread_local const std::vector<Stock>* drawing=nullptr;
thread_local void* modified_state=nullptr;
KilnStr str(const char* s){return {s,std::strlen(s)};}
void report(const char* name,uint32_t state,uint32_t reason,const char* why){
 const auto* caps=static_cast<const KilnCapsApi*>(api?api->service(str(KILN_CAPS_SERVICE),1):nullptr);
 if(caps&&caps->report)caps->report(items_owner(),str(name),state,reason,str(why));
}
void fail(const char* name,const char* why){report(name,KILN_CAP_UNAVAILABLE,KILN_CAP_REASON_FAULTED,why);}
bool read(void* out,const void* p,size_t n){__try {if(!p)return false;std::memcpy(out,p,n);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool write(void* p,const void* in,size_t n){__try {if(!p)return false;std::memcpy(p,in,n);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
template<class T> bool load(const void* p,size_t off,T* out){return p&&reinterpret_cast<uintptr_t>(p)<=UINTPTR_MAX-off&&read(out,static_cast<const uint8_t*>(p)+off,sizeof(T));}
// Validate the very schema the input sender/receiver uses, not merely the reflected C++ type. A narrowed slot index
// would invalidate the reserved-key protocol. All pointers and strings are read under the exception guard above.
bool named(const void* p,const char* wanted){char text[64]{};size_t n=std::strlen(wanted);return n<sizeof text&&read(text,p,n+1)&&std::memcmp(text,wanted,n+1)==0;}
uintptr_t field(uintptr_t schema,const char* name){
 uintptr_t data=0;uint64_t n=0;
 if(!load(reinterpret_cast<void*>(schema),16,&data)||!load(reinterpret_cast<void*>(schema),24,&n)||n>64||!data)return 0;
 for(uint64_t i=0;i<n;++i){uintptr_t p=0;const uintptr_t f=data+i*64;if(!load(reinterpret_cast<void*>(f),0,&p))return 0;if(named(reinterpret_cast<void*>(p),name))return f;}
 return 0;
}
bool wire_valid(){
 uintptr_t root=0;if(!load(reinterpret_cast<void*>(wire_at),0,&root))return false;
 for(const char* name:{"newestEntry","data","versionedActions","itemAction","secondarySlotId"}){
  const uintptr_t f=field(root,name);uint32_t kind=0;
  if(!f||!load(reinterpret_cast<void*>(f),8,&kind)||kind!=12||!load(reinterpret_cast<void*>(f),56,&root))return false;
 }
 uintptr_t f=field(root,"slotIndex");uint32_t kind=0,repeat=0;uint64_t off=0,n=0,bits=0;
 return f&&load(reinterpret_cast<void*>(f),8,&kind)&&kind==4&&load(reinterpret_cast<void*>(f),16,&repeat)&&repeat==1&&
  load(reinterpret_cast<void*>(f),24,&off)&&off==4&&load(reinterpret_cast<void*>(f),40,&n)&&n==1&&load(reinterpret_cast<void*>(f),48,&bits)&&bits==0;
}
// Tint's access set is intentionally sparse: Fuel is not present. Validate every accessor we use, on every request;
// do not use access_from_tx's full-set cache or a world/singleton lookup on a game worker thread.
bool validate_access(Access* a){
 __try {
  if(!a->ctx||!E.job_storage||!E.component_map_find)return false;
  a->job=*static_cast<void**>(a->ctx);if(!a->job)return false;
  a->storage=E.job_storage(a->job);
  if(!a->storage||a->storage!=active_server_storage.load(std::memory_order_acquire)||get<uint8_t>(a->storage,8)!=0||!a->items)return false;
  void* registry=*static_cast<void**>(a->storage);if(!registry)return false;
  for(int i:{A_INVENTORY,A_EMPTY,A_SETUP,A_ITEMSTATE,A_OWNER,A_SLOTSEL}){
   if(!a->acc[i])return false;
   const auto* type=E.component_map_find(static_cast<uint8_t*>(registry)+E.registry_components,&kAccHashes[i]);
   if(!type||*reinterpret_cast<const uint16_t*>(a->acc[i])!=*type)return false;
  }
  return true;
 }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool entry_access(const uint64_t* it,Access* a,uint32_t* player,void** bases){
 __try {
  a->ctx=reinterpret_cast<void*>(it[0]);a->items=reinterpret_cast<void*>(it[0xd8/8]);
  a->acc[A_INVENTORY]=it[0x148/8];a->acc[A_EMPTY]=it[0x158/8];a->acc[A_SETUP]=it[0x160/8];
  a->acc[A_ITEMSTATE]=it[0x168/8];a->acc[A_OWNER]=it[0x178/8];a->acc[A_SLOTSEL]=it[0x190/8];
  if(!validate_access(a))return false;
  alignas(16) uint8_t out[64]{};const uint32_t* who=actor_fn(a->ctx,out);
  if(!who||!*who||*who>1023)return false;
  *player=*who;*bases=reinterpret_cast<void*>(it[0xc8/8]);return *bases!=nullptr;
 }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool tx_access(void* tx,Access* a){
 __try {
  const auto* w=static_cast<uint64_t*>(tx);a->ctx=reinterpret_cast<void*>(w[0]);a->items=reinterpret_cast<void*>(w[1]);
  a->acc[A_INVENTORY]=w[4];a->acc[A_EMPTY]=w[0x14];a->acc[A_SETUP]=w[3];a->acc[A_ITEMSTATE]=w[5];
  a->acc[A_OWNER]=w[0x12];a->acc[A_SLOTSEL]=w[0x1d];return validate_access(a);
 }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool guards(void* tx){__try {return !(get<uint8_t>(tx,0xb0)&0x40)&&get<uintptr_t>(tx,0xa0)!=0;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool dye_item(const Access& a,uint32_t item){
 __try {const auto* info=static_cast<uint8_t*>(E.item_find(item,a.items));return info&&info[0xcc]==16&&info[0x748]!=0;}
 __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
uint64_t find_slot(const Access& a,uint32_t holder,uint32_t item){
 PageSet pages{};bool init=false;
 if(!read_pages(a,holder,&pages,&init)||!init)return 0;
 for(uint32_t i=0;i<pages.n;++i)for(const auto& s:pages.p[i].s)if(s.item==item&&s.count)return s.slot;
 return 0;
}
bool resolve_action(const uint64_t* action,const uint64_t* it,uint32_t item,uint64_t* copy,Route* active){
 try {
  Access a{};uint32_t player=0;void* bases=nullptr;
  if(!entry_access(it,&a,&player,&bases)||!dye_item(a,item))return false;
  uint64_t slot=find_slot(a,player,item);
  if(!slot)for(const auto& s:craft_sources_copy(player))if(s.allows(item)&&craft_source_access(bases,player,s.container)){
   slot=find_slot(a,s.container,item);if(slot)break;
  }
  if(!slot||!load(action,0,&copy[0]))return false;
  copy[1]=slot;*active={player,item,a.ctx,a.storage};return true;
 }catch(...){faulted.store(true);return false;}
}
void call_entry(const uint64_t* action,const uint64_t* it,Route* active){
 Route* previous=route;route=active;
 __try {entry_hook.original<EntryFn>()(action,it);}
 __finally {route=previous;}
}
void hk_entry(const uint64_t* action,const uint64_t* it){
 uint64_t secondary=0;uint32_t item=0;
 if(!load(action,8,&secondary)||!token_item(secondary,&item))return entry_hook.original<EntryFn>()(action,it);
 if(!server_ok.load()||faulted.load())return;
 uint64_t copy[2]{};Route active{};
 if(resolve_action(action,it,item,copy,&active))call_entry(copy,it,&active);
}
Result12* take_guarded(Result12* out,void* tx,uint32_t holder,uint32_t item){
 __try {if(pay_hook.original<RemoveItemsFn>()(out,tx,holder,item,uint64_t(0),uint32_t(1))!=out)*out={0x27,{0,0,0},0,0};return out;}
 __except(EXCEPTION_EXECUTE_HANDLER){*out={0x27,{0,0,0},0,0};faulted.store(true);return out;}
}
Result12* hk_pay(Result12* out,void* tx,uint32_t owner,uint32_t item,uint64_t preferred,uint32_t amount){
 const uintptr_t caller=pay_hook.caller();
 if(!route||caller!=pay_ret)return pay_hook.original<RemoveItemsFn>()(out,tx,owner,item,preferred,amount);
 *out={0x27,{0,0,0},0,0};
 try {
  Access a{};void* bases=nullptr;
  if(faulted.load()||owner!=route->player||item!=route->item||amount!=1||!tx_access(tx,&a)||a.ctx!=route->ctx||a.storage!=route->storage||
     !load(tx,0x70,&bases)||!bases||!guards(tx))return out;
  const auto registrations=craft_sources_copy(owner);std::vector<uint32_t> sources;
  for(const auto& s:registrations)if(s.allows(item)&&std::find(sources.begin(),sources.end(),s.container)==sources.end())sources.push_back(s.container);
  const Payment result=pay_one(owner,sources,[&](uint32_t container){
   if(!craft_source_access(bases,owner,container))return false;
   PageSet pages{};bool init=false;return read_pages(a,container,&pages,&init)&&init&&!(pages.flags&3);
  },[&](uint32_t holder){Result12 got{};take_guarded(&got,tx,holder,item);return Payment{got.error,got.value};},[&]{return !faulted.load()&&guards(tx);});
  out->error=result.error;out->value=result.left;out->zero=0;
 }catch(...){faulted.store(true);}
 return out;
}

std::vector<Stock> current_offer(uint32_t player){
 std::lock_guard lock(offer_mu);
 if(!offer_allowed(client_world.load()!=0,offer.player==player&&offer.epoch==client_epoch.load(),offer.expires>GetTickCount64()))return {};
 return offer.stock;
}
bool ui_player(uint32_t* player){
 __try {const auto app=get<uintptr_t>(reinterpret_cast<void*>(app_at),0);const auto data=get<uintptr_t>(reinterpret_cast<void*>(app),0xd0);
  const auto state=get<uintptr_t>(reinterpret_cast<void*>(data),0);if(!state||get<uint8_t>(reinterpret_cast<void*>(state),0xc)!=KILN_UI_STATE_PLAY)return false;
  const auto play=get<uintptr_t>(reinterpret_cast<void*>(state),0x38);if(!play||get<uint8_t>(reinterpret_cast<void*>(play),0x1cd8)!=42)return false;
  *player=get<uint32_t>(reinterpret_cast<void*>(play),0x18);return *player!=0;
 }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool rebuild(void* state){__try {build_fn(static_cast<uint8_t*>(state)+0x90b80);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool replace_rows(void* state,const std::vector<View>& rows,uint32_t selected){
 __try {
  auto* h=static_cast<uint8_t*>(state)+0x90b80;
  uint64_t n=rows.size();
  if(get<uint64_t>(h,16)<n){auto grow=reinterpret_cast<GrowFn>(get<uintptr_t>(h,24));if(!grow||!grow(h,-static_cast<int64_t>(n),0))return false;}
  const auto p=get<uintptr_t>(h,0);if(!p||get<uint64_t>(h,16)<n)return false;
  if(n)std::memcpy(reinterpret_cast<void*>(p),rows.data(),n*view_bytes);
  put(h,8,n);uintptr_t found=0;
  for(size_t i=0;i<rows.size();++i)if(id(rows[i])==selected){found=p+i*view_bytes;break;}
  put(state,8,found);return true;
 }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void draw_grid(void* state,const std::vector<Stock>* stock){
 const std::vector<Stock>* previous=drawing;drawing=stock;
 __try {grid_hook.original<GridFn>()(state);}
 __finally {drawing=previous;}
}
void hk_grid(void* state){
 if(!ui_ok.load()&&modified_state!=state)return grid_hook.original<GridFn>()(state);
 try {
  uint32_t player=0;if(!ui_player(&player))return grid_hook.original<GridFn>()(state);
  auto stock=ui_ok.load()?current_offer(player):std::vector<Stock>{};
  if(stock.empty()&&modified_state!=state)return grid_hook.original<GridFn>()(state);
  uintptr_t selected_ptr=0;uint32_t selected=0;
  if(load(state,8,&selected_ptr)&&selected_ptr)load(reinterpret_cast<void*>(selected_ptr),8,&selected);
  if(!rebuild(state))throw 1;
  uintptr_t data=0;uint64_t count=0;
  if(!load(state,0x90b80,&data)||!load(state,0x90b88,&count)||count>4096||(!data&&count))throw 1;
  std::vector<View> rows(static_cast<size_t>(count));
  if(count&&!read(rows.data(),reinterpret_cast<void*>(data),rows.size()*view_bytes))throw 1;
  merge(&rows,stock);if(!replace_rows(state,rows,selected))throw 1;
  modified_state=stock.empty()?nullptr:state;
  draw_grid(state,&stock);
 }catch(...){ui_ok.store(false);fail("crafting.dye_ui","a dye palette view faulted; source rows are disabled until restart");}
}
void on_row(KilnRegisters& r){if(drawing){uint32_t item=0;if(load(reinterpret_cast<void*>(r.rbx),8,&item))r.rax=available(static_cast<uint32_t>(r.rax),extra(*drawing,item));}}
void on_detail(KilnRegisters& r){if(drawing)r.rax=available(static_cast<uint32_t>(r.rax),extra(*drawing,static_cast<uint32_t>(r.r15)));}
void KILN_CALL client_pre(const KilnTickInfo*,void*){
 const uintptr_t world=api->world(KILN_WORLD_CLIENT);
 if(client_world.exchange(world)!=world){client_epoch.fetch_add(1);std::lock_guard lock(offer_mu);offer={};}
 bool wanted=false;{std::lock_guard lock(offer_mu);wanted=offer.player!=0&&offer.expires>GetTickCount64();}
 if(wanted&&ui_feature&&!ui_ok.load()&&!grid_hook){
  if(!wire_valid()||!row_hook.install(api,items_owner(),row_at,&on_row,"dyes/client.RowCount")||
     !detail_hook.install(api,items_owner(),detail_at,&on_detail,"dyes/client.DetailCount")||
     !grid_hook.install(api,items_owner(),grid_at,reinterpret_cast<void*>(&hk_grid),"dyes/client.PaletteGrid")){
   row_hook.revoke();detail_hook.revoke();fail("crafting.dye_ui","the qualified dye palette hooks could not be installed");ui_feature=false;
  }else ui_ok.store(true);
 }
 report("crafting.dye_ui_session",wanted&&ui_ok.load()?KILN_CAP_AVAILABLE:KILN_CAP_PENDING,wanted&&ui_ok.load()?KILN_CAP_REASON_NONE:KILN_CAP_REASON_SESSION,
        wanted?"waiting for the dye palette hooks":"no live dye-source announcement from a Kiln server");
}
void KILN_CALL server_pre(const KilnTickInfo*,void*){
 const uintptr_t world=api->world(KILN_WORLD_SERVER);
 const bool live=world!=0;
 active_server_storage.store(world?reinterpret_cast<void*>(world+E.world_storage):nullptr,std::memory_order_release);
 report("crafting.dye_sources_session",live&&server_ok.load()&&!faulted.load()?KILN_CAP_AVAILABLE:KILN_CAP_PENDING,
        live&&server_ok.load()&&!faulted.load()?KILN_CAP_REASON_NONE:KILN_CAP_REASON_SESSION,"waiting for an available server world");
 if(faulted.load())fail("crafting.dye_sources","a source payment faulted; all source dye requests are refused until restart");
}
void KILN_CALL world_event(const KilnWorldEvent* e,void*){
 if(e->event!=KILN_WORLD_STOPPING)return;
 if(e->kind==KILN_WORLD_CLIENT){
  client_world.store(0);client_epoch.fetch_add(1);{std::lock_guard lock(offer_mu);offer={};}
  report("crafting.dye_ui_session",KILN_CAP_PENDING,KILN_CAP_REASON_SESSION,"no client world or live dye-source announcement");
 }
 if(e->kind==KILN_WORLD_SERVER){active_server_storage.store(nullptr,std::memory_order_release);report("crafting.dye_sources_session",KILN_CAP_PENDING,KILN_CAP_REASON_SESSION,"no server world");}
}
bool symbol(const char* name,uintptr_t* out){return api->symbol(str(name),out)==KILN_OK&&*out;}
bool layout_is(const char* name,int64_t expected){int64_t got=0;return api->constant(str(name),&got)==KILN_OK&&got==expected;}
bool server_layout(){
 return layout_is("dyes/action.secondary",8)&&layout_is("dyes/item.palette_set",0x748)&&layout_is("items/item.category",0xcc)&&
  layout_is("dyes/it.setup",0x160)&&layout_is("dyes/it.inventory",0x148)&&layout_is("dyes/it.selection",0x190)&&
  layout_is("dyes/it.item_state",0x168)&&layout_is("dyes/it.owner",0x178)&&layout_is("dyes/it.empty",0x158)&&
  layout_is("dyes/it.bases_server",0xc8)&&layout_is("items/tx.guard_flags",0xb0)&&layout_is("items/tx.empty_state",0xa0)&&
  layout_is("items/tx.touched_count",0x7148);
}
} // namespace

KilnResult KILN_CALL dyes_set_stock(KilnEntity player,const KilnStationStack* rows,uint32_t count){
 try {
 if(!api||api->side()!=KILN_SIDE_CLIENT)return KILN_ERR_UNAVAILABLE;
 if(count>max_rows||(!rows&&count)||(!player&&count))return KILN_ERR_INVALID_ARGUMENT;
 if(!player||!count){std::lock_guard lock(offer_mu);offer={};return KILN_OK;}
 if(!ui_feature||!client_world.load())return KILN_ERR_UNAVAILABLE;
 const uint64_t epoch=client_epoch.load();
 std::vector<Stock> stock;stock.reserve(count);
 for(uint32_t i=0;i<count;++i){KilnStationStack r{};if(!read(&r,rows+i,sizeof r)||!r.item||!r.count)return KILN_ERR_INVALID_ARGUMENT;
  if(std::any_of(stock.begin(),stock.end(),[&](const Stock& s){return s.item==r.item;}))return KILN_ERR_INVALID_ARGUMENT;
  stock.push_back({r.item,r.count});}
 std::lock_guard lock(offer_mu);
 if(!client_world.load()||epoch!=client_epoch.load())return KILN_ERR_UNAVAILABLE;
 offer={player,epoch,GetTickCount64()+15000,std::move(stock)};return KILN_OK;
 }catch(...){return KILN_ERR_INTERNAL;}
}
void dyes_resolve(){
 api=items_api();uintptr_t actor=0,build=0,entry=0,pay=0;
 server_feature=api->feature(str("crafting.dye_sources"))&&server_layout()&&E.read_ok&&E.tx_ok&&symbol("dyes/TintAction",&entry)&&
  symbol("items/InventoryRemoveItems",&pay)&&symbol("dyes/TintPayRet",&pay_ret)&&symbol("dyes/ProgramEntity",&actor)&&symbol("dyes/WireRoot",&wire_at)&&wire_valid();
 actor_fn=reinterpret_cast<ActorFn>(actor);
 if(api->side()==KILN_SIDE_CLIENT){
  ui_feature=api->feature(str("crafting.dye_ui"))&&layout_is("dyes/ui.palettes",0x90b80)&&layout_is("dyes/ui.palette_count",0x90b88)&&symbol("dyes/client.PaletteGrid",&grid_at)&&symbol("dyes/client.PaletteBuild",&build)&&
   symbol("dyes/client.RowCount",&row_at)&&symbol("dyes/client.DetailCount",&detail_at)&&symbol("ui/UiApp",&app_at)&&symbol("dyes/WireRoot",&wire_at)&&wire_valid();
  build_fn=reinterpret_cast<BuildFn>(build);
 }else report("crafting.dye_ui",KILN_CAP_UNAVAILABLE,KILN_CAP_REASON_WRONG_SIDE,"the dye palette screen runs only in a game client");
 if(!server_feature)report("crafting.dye_sources",KILN_CAP_UNAVAILABLE,KILN_CAP_REASON_SYMBOL_MISSING,"the tint transaction, sparse accessors or full-u8 input schema did not qualify");
 if(api->side()==KILN_SIDE_CLIENT&&!ui_feature)report("crafting.dye_ui",KILN_CAP_UNAVAILABLE,KILN_CAP_REASON_SYMBOL_MISSING,"the palette screen or full-u8 input schema did not qualify");
 report("crafting.dye_sources_session",KILN_CAP_PENDING,KILN_CAP_REASON_SESSION,"no server world yet");
 report("crafting.dye_ui_session",KILN_CAP_PENDING,KILN_CAP_REASON_SESSION,"no live dye-source announcement from a Kiln server");
}
void dyes_register(){
 uintptr_t entry=0,pay=0;
 if(server_feature&&symbol("dyes/TintAction",&entry)&&symbol("items/InventoryRemoveItems",&pay)){
  // Install the payment link first: the entry must never translate a reserved key without its authoritative debit.
  if(pay_hook.install(api,items_owner(),pay,reinterpret_cast<void*>(&hk_pay),"dyes/TintPay")&&
     entry_hook.install(api,items_owner(),entry,reinterpret_cast<void*>(&hk_entry),"dyes/TintAction"))server_ok.store(true);
  else fail("crafting.dye_sources","the qualified tint entry/payment hooks could not be installed");
 }
 KilnHandle h=0;
 api->subscribe_world_tick(items_owner(),KILN_WORLD_SERVER,KILN_TICK_PRE,&server_pre,nullptr,&h);
 if(api->side()==KILN_SIDE_CLIENT)api->subscribe_world_tick(items_owner(),KILN_WORLD_CLIENT,KILN_TICK_PRE,&client_pre,nullptr,&h);
 api->subscribe_world(items_owner(),&world_event,nullptr,&h);
}
} // namespace kitems
