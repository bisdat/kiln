// SPDX-License-Identifier: MIT
// GitHub issue 140: a user's pick of a mod row the game does not equip shows the HUD list again, as the game's own pick
// does once the host answers it. The fixture lays the selector groups out inside a G38UiPlayData-shaped buffer (groups
// at +0x14fd0, the active group's index after the seven groups, the stock-cycle count at +0x2fe8 on client 26430625)
// and calls the pick detour from the game's call sites by return address. No game code runs.
#define main presentation_fixture
#include "presentation_api.cpp"
#undef main
#include "../ammo_runtime.h"
#include <new>
#include <vector>
static KilnClientState ammo_client_state;
static KilnResult KILN_CALL ammo_state(KilnClientState* out){*out=ammo_client_state;return KILN_OK;}
static bool KILN_CALL ammo_thread(){return true;}
static int backpack_answer;
static int backpack_fixture(uint32_t,uint32_t){return backpack_answer;}
static unsigned equip_calls;
static void KILN_CALL equip_fixture(SelectorGroup*,void*,uint8_t,uint32_t,uint32_t){++equip_calls;}
static void KILN_CALL finalizer_fixture(SelectorGroup*,void*,void*,void*,void*,void*){}
static uint64_t count_999=9; // the game's count of its row 999 (0: the stack left, its row kept)
static void vanilla_selector(SelectorGroup* groups,void*,uint8_t) {
    for(unsigned i=0;i<7;++i) {
        auto& g=groups[i];g.data=g.inline_rows;g.capacity=64;g.count=2;
        g.data[0].item=999;g.data[0].count=count_999;g.data[1].item=1257792166u;g.data[1].count=3;
        if(!g.has_selected){g.has_selected=1;g.selected=0;}
    }
}
static void KILN_CALL ammo_selector_fixture(SelectorGroup* groups,void* access,uint8_t current){vanilla_selector(groups,access,current);}
static KilnResult KILN_CALL ammo_raw(KilnOwner,uintptr_t target,const KilnHookOrder*,void*,void** original,KilnHandle* handle) {
    *original=target==20?reinterpret_cast<void*>(&ammo_selector_fixture):target==21?reinterpret_cast<void*>(&equip_fixture):reinterpret_cast<void*>(&finalizer_fixture);
    *handle=target;return KILN_OK;
}
// The game's call sites of the pick, by return address (client 26430625): the user's relative cycle 0x3afb07 and
// absolute selection 0x3afa7a; the automatic ones 0x2bce29 (SelectInventoryStockItemEvent) and 0x2b484b; the building
// tool's material-from-crosshair pick 0x3afcd3; and 0x3a9617 / 0x3ade49 in functions nothing calls.
constexpr uintptr_t kPick=0x14040c150,kCycleRet=0x1403afb07,kSelectRet=0x1403afa7a,kEventRet=0x1402bce29,kAutoRet=0x1402b484b,
    kCrosshairRet=0x1403afcd3,kDeadRet1=0x1403a9617,kDeadRet2=0x1403ade49;
static uintptr_t game_caller;
static uintptr_t KILN_CALL fake_caller(KilnHandle){return game_caller;}
static std::map<std::string,uintptr_t> symbols;
static std::map<std::string,int64_t> constants;
static KilnResult KILN_CALL fake_symbol(KilnStr name,uintptr_t* out) {
    const auto i=symbols.find(std::string(name.ptr,name.len));if(i==symbols.end())return KILN_ERR_NOT_FOUND;*out=i->second;return KILN_OK;
}
static KilnResult KILN_CALL fake_constant(KilnStr name,int64_t* out) {
    const auto i=constants.find(std::string(name.ptr,name.len));if(i==constants.end())return KILN_ERR_NOT_FOUND;*out=i->second;return KILN_OK;
}
// sigdb's feature ui.ammo.reveal: every link of the chain (the watcher and the list draw too) resolved on this build.
static bool reveal_feature=true;
static bool KILN_CALL fake_feature(KilnStr name){return reveal_feature&&std::string(name.ptr,name.len)=="ui.ammo.reveal";}
// The image's bytes before each return site: E8 rel32 to the pick, unless a case says otherwise.
static uintptr_t call_target=kPick;
static uint8_t call_opcode=0xe8;
static KilnResult KILN_CALL call_bytes(uintptr_t at,void* out,size_t size) {
    if(size!=5)return KILN_ERR_INVALID_ARGUMENT;
    uint8_t b[5]{call_opcode};const int32_t rel=int32_t(int64_t(call_target)-int64_t(at+5));std::memcpy(b+1,&rel,4);
    std::memcpy(out,b,5);return KILN_OK;
}
constexpr size_t kGroups=0x14fd0,kCount=0x2fe8,kActive=kGroups+0x2bf8;
alignas(64) static uint8_t play[kActive+0x40];
static uint32_t& cycle_count(){return *reinterpret_cast<uint32_t*>(play+kCount);}
static uint8_t& active_group(){return play[kActive];}
int main() {
    presentation_fixture();
    fake_hooks.raw_detour=&ammo_raw;fake_hooks.caller=&fake_caller;fake_hooks.read_original=&call_bytes;fake_hooks.code_state=&code_state;
    qualification_state=KILN_CODE_ORIGINAL;
    KilnClientApi client_table{};client_table.state=&ammo_state;client_table.on_client_thread=&ammo_thread;
    KilnApi fake{};fake.size=sizeof fake;fake.abi_version=KILN_ABI_VERSION;fake.subscribe_tick=&tick;fake.unsubscribe=&unsubscribe;fake.service=&service;
    fake.symbol=&fake_symbol;fake.constant=&fake_constant;fake.feature=&fake_feature;
    api=&fake;hooks=&fake_hooks;child=88;retiring=99;
    ammo::client=&client_table;ammo::ready.store(true);ammo::backpack_probe=&backpack_fixture;
    ammo_client_state.size=sizeof ammo_client_state;ammo_client_state.side=KILN_SIDE_CLIENT;ammo_client_state.session=KILN_SESSION_HOST;
    ammo_client_state.generation=7;ammo_client_state.client_session=9;ammo_client_state.local_player=123;ammo_client_state.client_steps=10;
    CHECK(selector_hook.install(&fake,child,20,reinterpret_cast<void*>(&selector),"ammo selector fixture"));
    CHECK(ammo::pick_hook.install(&fake,child,21,reinterpret_cast<void*>(&ammo::pick),"ammo pick fixture"));
    CHECK(ammo::finalizer_hook.install(&fake,child,22,reinterpret_cast<void*>(&ammo::finalizer),"ammo finalizer fixture"));
    ammo::catalog[0]={999,1257792166u,555,556};ammo::catalog[1]={999,1257792166u,777,778};ammo::catalog_session=9;
    for(uint32_t i=0;i<KILN_TOOL_HOOK_COUNT;++i)kiln_tools_report_hook(i,1);
    auto tool=definition();KilnHandle tool_handle=0;CHECK(kiln_tools_define_family(7,&tool,&tool_handle)==KILN_OK);
    uint32_t item=tool.tool;const uint32_t* access[16]{};access[15]=&item;
    auto* const acc=const_cast<void*>(static_cast<const void*>(access));
    auto* groups=reinterpret_cast<SelectorGroup*>(play+kGroups);
    static_assert(sizeof(SelectorGroup)*7==0x2bf8);
    for(unsigned i=0;i<7;++i)new(groups+i) SelectorGroup{};
    active_group()=0;selector(groups,acc,0);
    // The provider replaces both lists: arrows 555/556 and spells 777/778 that the backpack does not hold, plus 999 the
    // game lists itself with stock.
    KilnUiAmmoRow arrows[]={{555,12,0,3},{556,4,0,4},{999,20,0,5}},spells[]={{777,1,0,6},{778,1,0,7},{999,20,0,8}};
    auto publish=[&](uint32_t kind,KilnUiAmmoRow* rows) {
        KilnUiAmmoContext context{};context.size=sizeof context;
        CHECK(ammo::context(7,kind,&context)==KILN_OK&&context.ok);
        KilnUiAmmoPublish pub{sizeof pub,kind,KUA_MODE_REPLACE,3,9,0,context.game_serial,rows};
        CHECK(ammo::publish(7,&pub)==KILN_OK);
    };
    publish(KUA_KIND_ARROW,arrows);publish(KUA_KIND_SPELL,spells);selector(groups,acc,0);
    CHECK(groups[0].data[0].item==555&&groups[1].data[0].item==777);
    KilnUiAmmoPick picks[64]{};uint32_t written=0;
    auto drain=[&]{CHECK(ammo::take_picks(7,picks,64,&written)==KILN_OK);return written;};
    // The game's own input: its inventoryStockCycleAction (+0x98/+0x9c) and the groups' pending identity (+0x63c/+0x640)
    // stay untouched by a synthetic pick.
    alignas(8) uint8_t input[952]{};const std::vector<uint8_t> input_before(input,input+sizeof input);
    auto pending_untouched=[&]{for(unsigned i=0;i<7;++i)for(unsigned b=0x639;b<0x648;++b)if(reinterpret_cast<uint8_t*>(groups+i)[b])return false;return true;};

    // Without the reveal qualified (no symbols yet) nothing reveals; the pick still reaches the provider.
    CHECK(!ammo::reveal_setup(kPick));
    backpack_answer=0;game_caller=kCycleRet;cycle_count()=41;
    ammo::pick(groups,input,0,555,100);CHECK(equip_calls==0&&cycle_count()==41&&drain()==1&&picks[0].item==555);

    symbols={{"ui/AmmoCycleReturn",kCycleRet},{"ui/AmmoSelectReturn",kSelectRet}};
    constants={{"ui/ammo.groups",int64_t(kGroups)},{"ui/ammo.cycle_count",int64_t(kCount)}};
    // A return site whose call is not to the pick, or not a call, does not qualify; nor do impossible layouts.
    call_target=kPick+0x10;CHECK(!ammo::reveal_setup(kPick));call_target=kPick;
    call_opcode=0xe9;CHECK(!ammo::reveal_setup(kPick));call_opcode=0xe8;
    qualification_state=KILN_CODE_FOREIGN;CHECK(!ammo::reveal_setup(kPick));qualification_state=KILN_CODE_ORIGINAL;
    constants["ui/ammo.cycle_count"]=kGroups;CHECK(!ammo::reveal_setup(kPick));
    constants["ui/ammo.cycle_count"]=kCount+1;CHECK(!ammo::reveal_setup(kPick));
    constants["ui/ammo.cycle_count"]=kCount;
    fake_hooks.caller=nullptr;CHECK(!ammo::reveal_setup(kPick));fake_hooks.caller=&fake_caller;
    symbols.erase("ui/AmmoSelectReturn");CHECK(!ammo::reveal_setup(kPick));symbols["ui/AmmoSelectReturn"]=kSelectRet;
    // The symbols and constants Kiln reads resolve, but another link of the chain did not (the list draw no longer fades
    // from the stamped time, or the watcher moved): the feature is off, so the count is never touched. Without a
    // feature query at all, the same.
    reveal_feature=false;CHECK(!ammo::reveal_setup(kPick));reveal_feature=true;
    fake.feature=nullptr;CHECK(!ammo::reveal_setup(kPick));fake.feature=&fake_feature;
    CHECK(ammo::reveal_setup(kPick));

    // A user's cycle to a mod row (arrows, the bow's list is the active one): the list shows once, the wish reaches the
    // provider once, and neither the game's equip request nor its pending selection is written.
    cycle_count()=41;equip_calls=0;
    ammo::pick(groups,input,0,555,101);
    CHECK(cycle_count()==42&&equip_calls==0&&drain()==1&&picks[0].item==555&&picks[0].kind==KUA_KIND_ARROW);
    // The other direction lands on the other mod row: once more, and the absolute selection the same.
    ammo::pick(groups,input,0,556,102);CHECK(cycle_count()==43&&drain()==1&&picks[0].item==556);
    game_caller=kSelectRet;ammo::pick(groups,input,0,555,103);CHECK(cycle_count()==44&&drain()==1&&picks[0].item==555);
    CHECK(std::equal(input_before.begin(),input_before.end(),input)&&pending_untouched());
    // Spells, with the staff's list active.
    active_group()=1;game_caller=kCycleRet;
    ammo::pick(groups,input,1,777,104);CHECK(cycle_count()==45&&drain()==1&&picks[0].item==777&&picks[0].kind==KUA_KIND_SPELL);
    ammo::pick(groups,input,1,778,105);CHECK(cycle_count()==46&&drain()==1&&picks[0].item==778);
    // A pick for a list the HUD does not show (the spell list while the bow's is active, or no active list): no reveal,
    // the wish still goes to the provider.
    active_group()=0;ammo::pick(groups,input,1,777,106);CHECK(cycle_count()==46&&drain()==1);
    active_group()=7;ammo::pick(groups,input,0,555,107);CHECK(cycle_count()==46&&drain()==1);
    active_group()=0;
    // The game's automatic selections never reopen the list.
    for(const uintptr_t from:{kEventRet,kAutoRet,kCrosshairRet,kDeadRet1,kDeadRet2,uintptr_t(0),kCycleRet+1}) {
        game_caller=from;ammo::pick(groups,input,0,555,108);CHECK(cycle_count()==46&&drain()==1);
    }
    game_caller=kCycleRet;
    // Unreadable backpack, a stale session: no event and no reveal.
    backpack_answer=-1;ammo::pick(groups,input,0,555,109);CHECK(cycle_count()==46&&equip_calls==0&&drain()==0);
    backpack_answer=0;++ammo_client_state.generation;ammo::pick(groups,input,0,555,110);
    CHECK(cycle_count()==46&&equip_calls==0&&drain()==0);--ammo_client_state.generation;
    // The player carries the stack: the game's own pick, exactly once; Kiln reveals nothing (the host's answer does).
    backpack_answer=1;ammo::pick(groups,input,0,555,111);CHECK(cycle_count()==46&&equip_calls==1&&drain()==0);
    // A row the game lists itself with stock: the game's, once.
    backpack_answer=0;ammo::pick(groups,input,0,999,112);CHECK(cycle_count()==46&&equip_calls==2&&drain()==0);
    // A row no mod provided: the game's, once.
    ammo::pick(groups,input,0,1257792166u,113);CHECK(cycle_count()==46&&equip_calls==3&&drain()==0);
    // A faulted adapter leaves picks to their old paths and reveals nothing.
    ammo::faulted.store(true);ammo::pick(groups,input,0,555,114);CHECK(cycle_count()==46);ammo::faulted.store(false);drain();
    // The HUD hides the list again by itself: Kiln counts one change per pick and nothing on its own ticks, builds or
    // finalizer.
    ammo::report_post(nullptr,nullptr);selector(groups,acc,0);ammo::finalizer(groups,nullptr,nullptr,nullptr,nullptr,nullptr);
    ammo::report_post(nullptr,nullptr);CHECK(cycle_count()==46);
    // The count wraps as the game's own increment does.
    cycle_count()=0xffffffffu;ammo::pick(groups,input,0,555,115);CHECK(cycle_count()==0&&drain()==1);
    CHECK(std::equal(input_before.begin(),input_before.end(),input)&&pending_untouched());
    // Append as Ui.AmmoList publishes it since 0.2.0-dev.31.12 (GitHub issue 145): the quiver's row of 999, which the game
    // lists with stock, sits behind the game's row; 556 is the quiver's own. 999's stack was in the backpack at the bind.
    {
        KilnUiAmmoRow quiver[]={{999,20,0,0},{556,4,0,1}};
        KilnUiAmmoContext context{};context.size=sizeof context;
        KilnUiAmmoPublish append{sizeof append,KUA_KIND_ARROW,KUA_MODE_APPEND,2,9,0,0,quiver};
        auto bind=[&] {
            selector(groups,acc,0);CHECK(ammo::context(7,KUA_KIND_ARROW,&context)==KILN_OK&&context.ok);
            append.serial=context.game_serial;CHECK(ammo::publish(7,&append)==KILN_OK);selector(groups,acc,0);
        };
        count_999=8;backpack_answer=1;bind(); // new rows, so the bind reads the backpack for 999
        CHECK(groups[0].count==3&&groups[0].data[0].item==999&&groups[0].data[0].count==8&&groups[0].data[2].item==556);
        active_group()=0;equip_calls=0;cycle_count()=50;drain();
        // The stack is there: the user's cycle and selection and the game's automatic selections are the game's, once
        // each; Kiln shows nothing (the host's answer does) and raises nothing.
        for(const uintptr_t from:{kCycleRet,kSelectRet,kEventRet,kAutoRet}){game_caller=from;ammo::pick(groups,input,0,999,130);}
        CHECK(equip_calls==4&&cycle_count()==50&&drain()==0);
        // The quiver's server half swapped 999 out before the game built its rows again: the user's picks raise Picked
        // and show the list, once each ...
        backpack_answer=0;game_caller=kCycleRet;ammo::pick(groups,input,0,999,131);
        CHECK(equip_calls==4&&cycle_count()==51&&drain()==1&&picks[0].item==999&&picks[0].row==0);
        game_caller=kSelectRet;ammo::pick(groups,input,0,999,132);CHECK(cycle_count()==52&&drain()==1);
        // ... the game's automatic selections raise it as in 0.2.0-dev.31.10 (the backpack lacks the stack) and show nothing.
        for(const uintptr_t from:{kEventRet,kAutoRet,kCrosshairRet}) {
            game_caller=from;ammo::pick(groups,input,0,999,133);CHECK(equip_calls==4&&cycle_count()==52&&drain()==1);
        }
        // After the game's rebuild (999 kept at count 0) and before Kiln.Ui's next publication: the same.
        count_999=0;selector(groups,acc,0);CHECK(groups[0].count==2&&groups[0].data[0].count==0);
        game_caller=kCycleRet;ammo::pick(groups,input,0,999,134);CHECK(equip_calls==4&&cycle_count()==53&&drain()==1);
        game_caller=kEventRet;ammo::pick(groups,input,0,999,135);CHECK(equip_calls==4&&cycle_count()==53&&drain()==1);
        // And bound again: the count-0 row as issue 131 decides it.
        bind();game_caller=kSelectRet;ammo::pick(groups,input,0,999,136);CHECK(equip_calls==4&&cycle_count()==54&&drain()==1);
        game_caller=kAutoRet;ammo::pick(groups,input,0,999,137);CHECK(equip_calls==4&&cycle_count()==54&&drain()==1);
        count_999=9;backpack_answer=0;game_caller=kCycleRet;
    }
    // stop() turns the reveal off with the rest.
    ammo::stop();cycle_count()=5;ammo::ready.store(true);ammo::catalog[0]={999,1257792166u,555,556};ammo::catalog_session=9;
    selector(groups,acc,0);publish(KUA_KIND_ARROW,arrows);selector(groups,acc,0);
    ammo::pick(groups,input,0,555,116);CHECK(cycle_count()==5&&drain()==1);
    ammo::retire(7);CHECK(kiln_tools_remove_family(7,tool_handle)==KILN_OK);
    ammo::stop();ammo::backpack_probe=nullptr;api=nullptr;child=retiring=0;
    std::cout<<"ammo HUD reveal: "<<checks<<" checks passed (includes presentation fixture)\n";
}
