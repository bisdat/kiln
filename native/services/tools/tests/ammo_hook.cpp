// SPDX-License-Identifier: MIT
#define main presentation_fixture
#include "presentation_api.cpp"
#undef main
#include "../ammo_runtime.h"
static KilnClientState ammo_client_state;
static KilnResult KILN_CALL ammo_state(KilnClientState* out){*out=ammo_client_state;return KILN_OK;}
static bool KILN_CALL ammo_thread(){return true;}
static int backpack_answer;
static int backpack_fixture(uint32_t,uint32_t){return backpack_answer;}
static unsigned equip_calls,finalizer_calls;
static void KILN_CALL equip_fixture(SelectorGroup*,void*,uint8_t,uint32_t,uint32_t){++equip_calls;}
static void KILN_CALL finalizer_fixture(SelectorGroup* g,void*,void*,void*,void*,void*){++finalizer_calls;g[0].selected=0;g[0].has_selected=1;}
static void vanilla_selector(SelectorGroup* groups,void*,uint8_t) {
    for(unsigned i=0;i<7;++i) {
        groups[i]={};groups[i].data=groups[i].inline_rows;groups[i].capacity=64;groups[i].count=2;
        groups[i].data[0].item=999;groups[i].data[0].count=9;groups[i].data[1].item=1257792166u;groups[i].data[1].count=3;
        groups[i].has_selected=1;groups[i].selected=0;
    }
}
static uint32_t held_ammo_item;
static uint32_t KILN_CALL ammo_held(){return held_ammo_item;}
static bool bad_rows;
static SelectorRow* readonly_rows;
static bool nested_ammo;
static SelectorGroup nested_groups[7];
static void KILN_CALL ammo_selector_fixture(SelectorGroup* groups,void* access,uint8_t current) {
    vanilla_selector(groups,access,current);
    if(bad_rows)groups[0].data=reinterpret_cast<SelectorRow*>(1);
    if(readonly_rows)groups[0].data=readonly_rows;
    if(nested_ammo){nested_ammo=false;selector(nested_groups,nullptr,0);}
}
static KilnResult KILN_CALL ammo_raw(KilnOwner,uintptr_t target,const KilnHookOrder*,void*,void** original,KilnHandle* handle) {
    *original=target==20?reinterpret_cast<void*>(&ammo_selector_fixture):target==21?reinterpret_cast<void*>(&equip_fixture):reinterpret_cast<void*>(&finalizer_fixture);
    *handle=target;return KILN_OK;
}
#ifdef _WIN32
static bool guarded_ammo(SelectorGroup* groups,void* access) {
    __try {selector(groups,access,0);return true;} __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
#endif
int main() {
    presentation_fixture();
    fake_hooks.raw_detour=&ammo_raw;
    KilnClientApi client_table{};client_table.state=&ammo_state;client_table.on_client_thread=&ammo_thread;
    KilnApi fake{};fake.size=sizeof fake;fake.abi_version=KILN_ABI_VERSION;fake.subscribe_tick=&tick;fake.unsubscribe=&unsubscribe;fake.service=&service;
    api=&fake;hooks=&fake_hooks;child=88;retiring=99;
    ammo::client=&client_table;ammo::ready.store(true);ammo::backpack_probe=&backpack_fixture;
    ammo_client_state.size=sizeof ammo_client_state;ammo_client_state.side=KILN_SIDE_CLIENT;ammo_client_state.session=KILN_SESSION_HOST;
    ammo_client_state.generation=7;ammo_client_state.client_session=9;ammo_client_state.local_player=123;ammo_client_state.client_steps=10;
    CHECK(selector_hook.install(&fake,child,20,reinterpret_cast<void*>(&selector),"ammo selector fixture"));
    CHECK(ammo::pick_hook.install(&fake,child,21,reinterpret_cast<void*>(&ammo::pick),"ammo pick fixture"));
    CHECK(ammo::finalizer_hook.install(&fake,child,22,reinterpret_cast<void*>(&ammo::finalizer),"ammo finalizer fixture"));
    ammo::catalog[0]={999,1257792166u,555};ammo::catalog[1]={999,1257792166u,555};ammo::catalog_session=9;
    for(uint32_t i=0;i<KILN_TOOL_HOOK_COUNT;++i)kiln_tools_report_hook(i,1);
    auto tool=definition();KilnHandle tool_handle=0;CHECK(kiln_tools_define_family(7,&tool,&tool_handle)==KILN_OK);
    held_ammo_item=tool.tool;kiln_tools_presentation_set_held_provider(&ammo_held);
    uint32_t item=tool.tool;const uint32_t* access[16]{};access[15]=&item;
    SelectorGroup groups[7];selector(groups,const_cast<void*>(static_cast<const void*>(access)),0);
    CHECK(groups[0].count==2&&groups[1].count==2);
    CHECK(groups[2].count==1&&groups[2].data[0].item==1257792166u); // No provider: all vanilla bytes/behavior retained.
    KilnUiAmmoContext context{};context.size=sizeof context;CHECK(ammo::context(7,KUA_KIND_ARROW,&context)==KILN_OK&&context.ok&&context.game_count==2);
    KilnUiAmmoRow supplied[]={{555,12,0,3},{999,20,0,4}};
    KilnUiAmmoPublish pub{sizeof pub,KUA_KIND_ARROW,KUA_MODE_REPLACE,2,9,0,context.game_serial,supplied};
    CHECK(ammo::publish(7,&pub)==KILN_OK);
    CHECK(ammo::publish(8,&pub)==KUA_ERR_BUSY);
    pub.serial++;CHECK(ammo::publish(7,&pub)==KILN_ERR_INVALID_ARGUMENT);pub.serial--;
    supplied[0].item=666;CHECK(ammo::publish(7,&pub)==KILN_ERR_INVALID_ARGUMENT);supplied[0].item=555;
    nested_ammo=true;selector(groups,const_cast<void*>(static_cast<const void*>(access)),0);
    CHECK(groups[0].count==2&&groups[0].data[0].item==555&&groups[0].data[0].count==12);
    CHECK(groups[2].count==1&&groups[2].data[0].item==1257792166u);
    CHECK(nested_groups[0].data[0].item==999); // Null-access nested call has no admission.
    groups[0].selected=1;groups[0].has_selected=1;selector(groups,const_cast<void*>(static_cast<const void*>(access)),0);
    CHECK(groups[0].selected==1&&groups[0].data[1].item==999); // identity, not old index
    groups[0].selected=0;groups[0].has_selected=1;std::swap(supplied[0],supplied[1]);
    CHECK(ammo::publish(7,&pub)==KILN_OK);selector(groups,const_cast<void*>(static_cast<const void*>(access)),0);
    CHECK(groups[0].selected==1&&groups[0].data[1].item==555);
    std::swap(supplied[0],supplied[1]);CHECK(ammo::publish(7,&pub)==KILN_OK);selector(groups,const_cast<void*>(static_cast<const void*>(access)),0);
    groups[0].selected=1;ammo::finalizer(groups,nullptr,nullptr,nullptr,nullptr,nullptr);CHECK(groups[0].selected==1&&finalizer_calls==1);
    backpack_answer=0;equip_calls=0;ammo::pick(groups,nullptr,0,555,100);CHECK(equip_calls==0);
    KilnUiAmmoPick picks[64]{};uint32_t written=99;CHECK(ammo::take_picks(7,picks,64,&written)==KILN_OK&&written==1&&picks[0].item==555&&picks[0].row==3);
    backpack_answer=1;ammo::pick(groups,nullptr,0,555,101);CHECK(equip_calls==1); // A real stack uses the game, even for a provider row.
    backpack_answer=-1;ammo::pick(groups,nullptr,0,555,102);CHECK(equip_calls==1); // Unreadable is never equip permission.
    CHECK(ammo::take_picks(7,picks,64,&written)==KILN_OK&&written==0);
    ammo::pick(groups,nullptr,1,999,103);CHECK(equip_calls==2); // Unprovided Spell unchanged.
    pub.mode=KUA_MODE_GAME;pub.count=0;pub.rows=nullptr;CHECK(ammo::publish(7,&pub)==KILN_OK);
    backpack_answer=0;ammo::pick(groups,nullptr,0,555,104);CHECK(equip_calls==2); // Removed provider's already drawn virtual row is still suppressed.
    selector(groups,const_cast<void*>(static_cast<const void*>(access)),0);CHECK(groups[0].data[0].item==999&&groups[0].count==2);
    CHECK(ammo::context(7,KUA_KIND_ARROW,&context)==KILN_OK);
    pub.mode=KUA_MODE_REPLACE;pub.count=2;pub.rows=supplied;pub.serial=context.game_serial;
    CHECK(ammo::publish(7,&pub)==KILN_OK);selector(groups,const_cast<void*>(static_cast<const void*>(access)),0);
    backpack_answer=0;for(unsigned i=0;i<70;++i)ammo::pick(groups,nullptr,0,555,200+i);
    CHECK(ammo::take_picks(8,picks,64,&written)==KILN_OK&&written==0);
    CHECK(ammo::take_picks(7,picks,64,&written)==KILN_OK&&written==64);
    ammo::pick(groups,nullptr,0,555,299);CHECK(ammo::publish(7,&pub)==KILN_OK);
    CHECK(ammo::take_picks(7,picks,64,&written)==KILN_OK&&written==0); // Old row keys cannot retarget new publications.
    selector(groups,const_cast<void*>(static_cast<const void*>(access)),0);
    ammo::pick(groups,nullptr,0,555,300);++ammo_client_state.client_session;
    CHECK(ammo::take_picks(7,picks,64,&written)==KILN_OK&&written==0);--ammo_client_state.client_session;
#ifdef _WIN32
    CHECK(guarded_ammo(groups,reinterpret_cast<void*>(1))); // Unreadable access never inherits outer admission.
    bad_rows=true;CHECK(guarded_ammo(groups,const_cast<void*>(static_cast<const void*>(access)))&&ammo::failed());bad_rows=false;
    CHECK(groups[0].count==2); // A read fault before any write does not mutate vanilla.
    ammo::faulted.store(false);selector(groups,const_cast<void*>(static_cast<const void*>(access)),0);
    auto* page=static_cast<SelectorRow*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(page);
    page[0].item=999;page[0].count=9;page[1].item=1257792166u;page[1].count=3;
    DWORD old_protection=0;CHECK(VirtualProtect(page,4096,PAGE_READONLY,&old_protection));readonly_rows=page;
    CHECK(guarded_ammo(groups,const_cast<void*>(static_cast<const void*>(access)))&&ammo::failed()&&groups[0].count==0);
    readonly_rows=nullptr;CHECK(VirtualFree(page,0,MEM_RELEASE));ammo::faulted.store(false);
#endif
    ammo::retire(7);CHECK(ammo::take_picks(7,picks,64,&written)==KILN_OK&&written==0);
    CHECK(kiln_tools_remove_family(7,tool_handle)==KILN_OK);kiln_tools_presentation_set_held_provider(nullptr);
    ammo::stop();ammo::backpack_probe=nullptr;api=nullptr;child=retiring=0;
    std::cout<<"ammo native hooks: "<<checks<<" checks passed (includes presentation fixture)\n";
}
