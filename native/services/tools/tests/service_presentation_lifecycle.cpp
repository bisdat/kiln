// SPDX-License-Identifier: MIT
// Exercise the real service entry/ready/shutdown and presentation installer together. The other native
// packages and host are recording fakes: no game is launched and no executable memory is patched.
#include "../tools.cpp"
#include "services/kiln_client.h"
#include "services/kiln_world.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
namespace fixture {
constexpr KilnOwner parent=66,child=88;
std::vector<std::string> events;
std::map<std::string,uintptr_t> addresses;
std::map<uintptr_t,std::vector<uint8_t>> originals;
KilnHooksApi hooks{};
KilnClientApi client{};
KilnWorldApi world_service{};
uint64_t KILN_CALL world_session(uint32_t){return 0;}
KilnResult KILN_CALL entity_open(KilnOwner,uint64_t,uint32_t,uint64_t,KilnEntity,KilnObjectRef*){return KILN_ERR_UNAVAILABLE;}
KilnResult KILN_CALL snapshot(KilnOwner,const KilnObjectRef*,uint32_t,void*,uint32_t,uint32_t*){return KILN_ERR_UNAVAILABLE;}
bool drained=false,observer_tick=false,observer_world=false;
unsigned hook_retirements=0,owner_retirements=0;
std::string text(KilnStr s) {return {s.ptr,s.len};}
void record(const char* s) {events.emplace_back(s);}
void bytes(const char* name,uintptr_t address,const char* pattern) {
    addresses[name]=address;auto& out=originals[address];
    for(const char* p=pattern;*p;) {
        if(*p==' '){++p;continue;}
        out.push_back(*p=='?'?0:uint8_t(std::strtoul(std::string(p,2).c_str(),nullptr,16)));p+=2;
    }
}
KilnResult KILN_CALL build(KilnBuildInfo* out) {
    *out={};out->size=sizeof *out;out->side=KILN_SIDE_CLIENT;out->verified=1;out->hooks_enabled=1;
    std::strcpy(out->exe_sha256,"5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77");return KILN_OK;
}
const void* KILN_CALL service(KilnStr name,uint32_t) {
    const auto s=text(name);if(s==KILN_HOOKS_SERVICE)return &hooks;if(s==KILN_CLIENT_SERVICE)return &client;if(s==KILN_WORLD_SERVICE)return &world_service;return nullptr;
}
KilnResult KILN_CALL symbol(KilnStr name,uintptr_t* out) {
    const auto i=addresses.find(text(name));if(i==addresses.end())return KILN_ERR_NOT_FOUND;*out=i->second;return KILN_OK;
}
KilnResult KILN_CALL read_original(uintptr_t address,void* out,uint64_t size) {
    for(const auto& [base,data]:originals)if(address>=base&&address-base<=data.size()&&size<=data.size()-(address-base)) {
        std::memcpy(out,data.data()+address-base,size);return KILN_OK;
    }
    return KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL code_state(uintptr_t,uint64_t,uint32_t* state,uint64_t* generation) {*state=KILN_CODE_ORIGINAL;*generation=1;return KILN_OK;}
KilnOwner KILN_CALL owner_create(KilnStr name) {CHECK(text(name)=="kiln.tools.presentation");record("presentation child");return child;}
KilnResult KILN_CALL retiring(KilnOwner owner,KilnOwnerRetiringFn,void*,KilnHandle* out) {CHECK(owner==child);*out=99;record("retiring subscription");return KILN_OK;}
KilnResult KILN_CALL raw(KilnOwner owner,uintptr_t target,const KilnHookOrder*,void*,void** original,KilnHandle* out) {
    CHECK(owner==child);
    for(const auto& [name,address]:addresses)if(address==target)events.push_back(name);
    *original=reinterpret_cast<void*>(target);*out=target;return KILN_OK;
}
KilnResult KILN_CALL mid(KilnOwner owner,uintptr_t target,const KilnHookOrder*,KilnMidFn,void*,KilnHandle* out,uint32_t* unchecked) {
    CHECK(owner==child&&target==addresses["tools/BrushQueryFrame"]);CHECK(observer_tick&&observer_world);
    record("tools/BrushQueryFrame");*out=target;*unchecked=0;return KILN_OK;
}
KilnResult KILN_CALL revoke(KilnHandle) {return KILN_OK;}
KilnResult KILN_CALL revoke_hooks(KilnOwner owner) {
    CHECK(drained&&owner==child&&!observer_tick&&!observer_world);
    for(auto id:{KILN_TOOL_HOOK_MATERIAL_LOOKUP,KILN_TOOL_HOOK_SELECTOR_BUILDER,KILN_TOOL_HOOK_PREVIEW,KILN_TOOL_HOOK_BRUSH_PICKER,KILN_TOOL_HOOK_IDENTITY})CHECK(!kiln_tools_hook_installed(id));
    ++hook_retirements;record("presentation hooks retired");return KILN_OK;
}
void KILN_CALL revoke_owner(KilnOwner owner) {CHECK(owner==child&&hook_retirements==1);++owner_retirements;record("presentation owner retired");}
bool KILN_CALL feature(KilnStr name) {return text(name)=="world.lifecycle";}
KilnResult KILN_CALL world(KilnOwner owner,KilnWorldEventFn,void*,KilnHandle* out) {
    CHECK(owner==parent||owner==child);*out=owner==parent?1:3;
    if(owner==child){observer_world=true;record("held world");}return KILN_OK;
}
KilnResult KILN_CALL tick(KilnOwner owner,int32_t phase,KilnTickFn,void*,KilnHandle* out) {CHECK(owner==parent&&phase==KILN_TICK_PRE);*out=2;return KILN_OK;}
KilnResult KILN_CALL unsubscribe(KilnHandle handle) {
    if(handle==3){CHECK(drained);observer_world=false;record("held world stopped");}return KILN_OK;
}
KilnResult KILN_CALL client_tick(KilnOwner owner,int32_t phase,KilnTickFn,void*,KilnHandle* out) {
    CHECK(owner==child&&phase==KILN_TICK_POST&&observer_world);observer_tick=true;*out=4;record("held Post");return KILN_OK;
}
KilnResult KILN_CALL client_unsubscribe(KilnHandle handle) {CHECK(drained&&handle==4);observer_tick=false;record("held Post stopped");return KILN_OK;}
KilnResult KILN_CALL state(KilnClientState* out) {*out={};out->size=sizeof *out;out->side=KILN_SIDE_CLIENT;out->flags=KILN_CLIENT_LIFECYCLE;return KILN_OK;}
bool KILN_CALL client_thread() {return true;}
uintptr_t KILN_CALL client_world() {return 0;}
KilnResult KILN_CALL register_service(KilnOwner owner,KilnStr name,const void*) {CHECK(owner==parent&&(text(name)==KILN_TOOLS_SERVICE||text(name)==KILN_UI_AMMO_SERVICE));return KILN_OK;}
size_t position(const char* event) {const auto i=std::find(events.begin(),events.end(),event);CHECK(i!=events.end());return size_t(i-events.begin());}
}
bool kiln_tools_impact_install(const KilnApi*,KilnOwner owner,KilnToolAreaCheck) noexcept {CHECK(owner==fixture::parent);fixture::record("impact");return true;}
void kiln_tools_impact_world(uintptr_t) noexcept {}
bool kiln_tools_impact_close() noexcept {fixture::record("close and drain");return fixture::drained;}
void kiln_tools_impact_uninstall() noexcept {CHECK(fixture::drained);fixture::record("stop impact");}
bool kiln_tools_actions_ready(uint32_t) noexcept {return false;}
bool kiln_tools_actions_faulted() noexcept {return false;}
uint64_t kiln_tools_actions_dispatched() noexcept {return 0;}
uint64_t kiln_tools_actions_refused() noexcept {return 0;}
bool kiln_tools_bind_programs(KilnOwner,const KilnToolFamilyDef&) noexcept {return true;}
void kiln_tools_unbind_programs(KilnOwner,uint32_t) noexcept {}
bool kiln_tools_build_area_init(const KilnApi*) {fixture::record("area");return true;}
bool kiln_tools_check_build_area(uintptr_t,uint32_t,const double[3],double) {return false;}
void kiln_tools_carve_install(const KilnApi*,KilnOwner) {fixture::record("carve");}
void kiln_tools_carve_uninstall() {CHECK(fixture::drained);fixture::record("stop carve");}
extern "C" bool kiln_tools_payment_install(const KilnApi*,KilnOwner) {fixture::record("payment");return true;}
extern "C" void kiln_tools_payment_uninstall() {CHECK(fixture::drained);fixture::record("stop payment");}
bool kiln_tools_history_install(const KilnApi*,KilnOwner) {fixture::record("history");return true;}
void kiln_tools_history_uninstall() {CHECK(fixture::drained);fixture::record("stop history");}
int main() {
    using namespace fixture;
    bytes("tools/TerrainConfigLookup",0x10000,"4C 8B 51 38 4D 85 D2 74 ?? 45 8B 5A 04 33 C9 4D 85 DB 74 ?? 4D 63 0A 45 33 C0 66 0F 1F 44 00 00 45 85 C9 75 ?? 33 C0 EB ?? 4B 8D 04 0A 49 03 C0 38 10 74 ?? 48 FF C1 49 81 C0 B8 00 00 00");
    bytes("tools/MaterialPreview",0x20000,"48 89 5C 24 20 4C 89 44 24 18 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 B0 F9 FF FF 48 81 EC 50 07 00 00");
    bytes("tools/RootIdentity",0x30000,"48 89 5C 24 20 56 48 83 EC 20 48 8B F1 48 85 D2 0F 84 ?? ?? ?? ?? 8B 82 D8 05 00 00");
    bytes("tools/MaterialSelector",0x40000,"48 89 5C 24 08 44 88 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 90 01 00 00 33 ED 4C 8D B9 38 06 00 00 48 8B DA 44 8B F5");
    bytes("frame prefix",0x50000,"0F 29 85 00 01 00 00 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8B 88 C8 00 00 00 48 81 C1 E0 A7 00 00 E8 ?? ?? ?? ?? 89 45 74");
    addresses["tools/BrushQueryFrame"]=0x50007;
    bytes("tools/BrushQuery",0x60000,"4C 89 44 24 18 48 89 54 24 10 55 53 56 41 54 41 55 41 57 48 8D 6C 24 D1 48 81 EC C8 00 00 00");
    const int32_t relative=0x60000-(0x50007+5);std::memcpy(originals[0x50000].data()+8,&relative,4);
    bytes("ui/AmmoItemLookup",0x80000,"89 4C 24 08 48 83 EC 28 85 C9 74 1B 48 8D 4A 38 48 8D 54 24 30 E8 ?? ?? ?? ?? 48 85 C0 74 08 48 8B 00");
    bytes("ui/AmmoItemMap",0x90000,"48 89 5C 24 08 48 89 7C 24 10 83 79 54 00 4C 8B C9 48 8B 59 10 74 53 44 8B 1A 48 8D 43 FF 48 8B 79 08 49 23 C3");
    const int32_t map_relative=0x90000-(0x80000+26);std::memcpy(originals[0x80000].data()+22,&map_relative,4);
    bytes("ui/AmmoFinalizer",0xa0000,"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 4C 8B 6D 50 49 8B F1 4C 8B 65 58");
    bytes("ui/AmmoPick",0xb0000,"4C 8B D9 41 80 F8 07 0F 83 91 00 00 00 48 8D 0D ?? ?? ?? ?? 41 0F B6 C0 8B 84 81 F0 C1 40 00 48 03 C1 FF E0");
    world_service.header={sizeof world_service,KILN_WORLD_VERSION};world_service.game_world_session=&world_session;world_service.entity_open=&entity_open;world_service.ecs_snapshot=&snapshot;
    addresses["GetComponent"]=0x70000; // Observer resolves this, but callbacks are not executed in this wiring test.
    fixture::hooks.header={sizeof fixture::hooks,KILN_HOOKS_VERSION};fixture::hooks.raw_detour=&raw;fixture::hooks.mid2_checked=&mid;
    fixture::hooks.read_original=&read_original;fixture::hooks.code_state=&code_state;fixture::hooks.subscribe_owner_retiring=&retiring;
    fixture::hooks.revoke=&revoke;fixture::hooks.revoke_owner=&revoke_hooks;
    client.header={sizeof client,KILN_CLIENT_VERSION};client.state=&state;client.subscribe_tick=&client_tick;client.unsubscribe=&client_unsubscribe;
    client.on_client_thread=&client_thread;client.client_world=&client_world;
    KilnApi host{};host.size=sizeof host;host.abi_version=KILN_ABI_VERSION;host.register_service=&register_service;
    host.service=&service;host.build_info=&build;host.symbol=&symbol;host.owner_create=&owner_create;host.owner_revoke=&revoke_owner;
    host.feature=&feature;host.subscribe_world=&fixture::world;host.subscribe_tick=&tick;host.unsubscribe=&unsubscribe;
    KilnPluginInfo info{};CHECK(kiln_plugin_main(&host,fixture::parent,&info)==KILN_OK);CHECK(events.empty());CHECK(info.on_ready&&info.on_unload);
    info.on_ready(nullptr);
    for(auto id:{KILN_TOOL_HOOK_MATERIAL_LOOKUP,KILN_TOOL_HOOK_SELECTOR_BUILDER,KILN_TOOL_HOOK_PREVIEW,KILN_TOOL_HOOK_BRUSH_PICKER,KILN_TOOL_HOOK_IDENTITY})CHECK(kiln_tools_hook_installed(id));
    CHECK(position("impact")<position("area")&&position("area")<position("carve"));
    CHECK(position("carve")<position("payment")&&position("payment")<position("history"));
    CHECK(position("history")<position("presentation child"));
    CHECK(position("retiring subscription")<position("tools/TerrainConfigLookup"));
    CHECK(position("tools/TerrainConfigLookup")<position("tools/MaterialPreview"));
    CHECK(position("held Post")<position("tools/MaterialSelector")&&position("tools/MaterialSelector")<position("tools/BrushQueryFrame"));
    CHECK(position("tools/MaterialSelector")<position("ui/AmmoFinalizer")&&position("ui/AmmoFinalizer")<position("ui/AmmoPick"));
    const auto installed=events.size();info.on_unload(nullptr);
    CHECK(events.size()==installed+1&&events.back()=="close and drain");CHECK(!hook_retirements&&!owner_retirements&&observer_tick&&observer_world);
    drained=true;info.on_unload(nullptr);
    CHECK(hook_retirements==1&&owner_retirements==1&&!observer_tick&&!observer_world);
    CHECK(position("held Post stopped")<position("presentation hooks retired"));
    CHECK(position("presentation owner retired")<position("stop history"));
    CHECK(position("stop history")<position("stop payment")&&position("stop payment")<position("stop carve")&&position("stop carve")<position("stop impact"));
    std::cout<<"service presentation lifecycle: "<<checks<<" checks passed\n";
}
