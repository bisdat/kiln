// SPDX-License-Identifier: MIT
#ifdef _WIN32
#include <windows.h>
#endif
#include "../impact_binding.h"
#include "../impact_args.h"
#include "../action_trace.h"
#include "../internal.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {
int calls=0,checks=0; bool allow_area=true,nest=false,fault=false;
using kiln::tools::ActionExit;
// What the fake game does inside an admitted stroke: the terrain kernel's notes, a missing undo record, its result code.
struct KernelPlan {bool active=false,removable=false;int removed=0;} kernel_plan;
bool undo_missing=false; uint8_t game_code=0;
uint8_t expected_type=0; uint32_t expected_policy=100;
KilnHooksApi hooks{};
uint32_t live_code=KILN_CODE_ORIGINAL;
bool wrong_original=false,original_unreadable=false,state_unreadable=false,retire_in_call=false;
KilnResult KILN_CALL read_original(uintptr_t address,void* out,size_t size) {
    constexpr uint8_t entry[]={0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x41,0x55,0x41,0x56,0x41,0x57};
    if(original_unreadable || address!=1 || size!=sizeof(entry))return KILN_ERR_UNAVAILABLE;
    std::memcpy(out,entry,size);if(wrong_original)static_cast<uint8_t*>(out)[0]^=1;
    return KILN_OK;
}
KilnResult KILN_CALL code_state(uintptr_t,size_t,uint32_t* state,uint64_t* generation) {
    if(state_unreadable)return KILN_ERR_UNAVAILABLE;
    *state=live_code;*generation=1;return KILN_OK;
}
KilnOwnerRetiringFn retiring=nullptr;
// The engine's impact iterator (ImpactContext +0x30): qword 0 is the ProgramContext, whose job (+0) names the storage of
// the world running the program at job + world/job.storage; that storage is world + world.storage (docs/re/build.md 7).
constexpr uintptr_t server_world=0x10000000;
constexpr int64_t job_storage=0x30,world_storage=0xe179c0;
bool constants_missing=false;
KilnResult KILN_CALL watch(KilnOwner,KilnOwnerRetiringFn fn,void*,KilnHandle* out){retiring=fn;*out=2;return KILN_OK;}
KilnResult KILN_CALL unwatch(KilnHandle){return KILN_OK;}
struct Fixture {
    std::array<kab::ImpactVariable,7> layout{};
    std::array<uint8_t,256> data{};
    std::array<uintptr_t,32> access{};
    std::array<uintptr_t,2> program_context{};
    alignas(8) std::array<uint8_t,0x40> job{};
    std::array<uint32_t,9> handles{};
    kab::NativeCtx ctx{}; kab::ArgSlice args{}; std::array<uint8_t,16> result{}; KilnHookCall2 call{};
    Fixture() {
        uint32_t types[]={0x737c8f86,0x9493a8c5,0x4835c9e9,0xc4b5300b,0x10e7bb7a,0xe89f7410,0x5bfc0dbb};
        uint16_t sizes[]={4,56,1,20,4,4,4},offsets[]={0,16,80,96,128,144,160};
        for(int i=0;i<7;++i){layout[i].type=types[i];layout[i].size=sizes[i];layout[i].offset=offsets[i];}
        layout[6].config_id=0x220e6b10;
        for(int i=0;i<6;++i)handles[i]=0xffff0000u|uint32_t(i);
        uint32_t actor=7,brush=42;float radius=2;
        std::memcpy(data.data(),&actor,4);std::memcpy(data.data()+160,&brush,4);std::memcpy(data.data()+144,&radius,4);data[80]=255;
        program_context[0]=reinterpret_cast<uintptr_t>(job.data());
        set_world(server_world);
        access[0]=reinterpret_cast<uintptr_t>(program_context.data());access[0x88/8]=0x5678;
        ctx.layout=reinterpret_cast<uint8_t*>(layout.data());ctx.layout_count=layout.size();ctx.data=data.data();ctx.data_size=data.size();ctx.iter_ctx=reinterpret_cast<uintptr_t>(access.data());ctx.program_id=100;
        args={handles.data(),9};call.size=sizeof(call);call.arg_count=3;call.args[0]=reinterpret_cast<uintptr_t>(result.data());call.args[1]=reinterpret_cast<uintptr_t>(&args);call.args[2]=reinterpret_cast<uintptr_t>(&ctx);
    }
    Fixture(const Fixture&)=delete;
    Fixture& operator=(const Fixture&)=delete;
    void set_world(uintptr_t world){const uintptr_t storage=world+uintptr_t(world_storage);std::memcpy(job.data()+job_storage,&storage,sizeof storage);}
};
void check_at(bool b,int line){if(!b){std::fprintf(stderr,"impact dispatch: check failed at line %d\n",line);std::abort();}++checks;}
#define check(x) check_at((x),__LINE__)
bool area(uintptr_t zones,uint32_t actor,const double p[3],double radius){check(zones==0x5678&&actor==7&&p[0]==0&&radius==2);return allow_area;}
uintptr_t KILN_CALL original(KilnHookCall2* call) {
    ++calls;auto* c=reinterpret_cast<kab::NativeCtx*>(call->args[2]);
    if(!c){check(!kiln_tools_active_policy());return call->args[0];} // a vanilla impact passed through unread
    auto* policy=kiln_tools_active_policy();check(expected_policy ? policy&&policy->policy_id==expected_policy : policy==nullptr);
    check(c->data[80]==expected_type);
    if(retire_in_call) {retire_in_call=false;retiring(2,nullptr);check(kiln_tools_active_policy()==policy && policy->pool_count==1 && policy->pool[0].item==55);}
    if(nest){nest=false;Fixture nested;nested.ctx.program_id=999;auto old=expected_policy;auto type=expected_type;expected_policy=0;expected_type=255;kiln_tools_impact_detour(&nested.call,nullptr);expected_policy=old;expected_type=type;check(kiln_tools_active_policy()&&kiln_tools_active_policy()->policy_id==old);}
#ifdef _WIN32
    if(fault)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
#else
    if(fault)throw 7;
#endif
    // As in the game: the carve and history hooks note only under an active Remove policy (a nested vanilla impact has none).
    const bool removing=kiln_tools_active_policy()&&kiln_tools_active_policy()->mode==KILN_TOOL_POLICY_REMOVE;
    if(removing&&kernel_plan.active)kiln::tools::note_kernel(kernel_plan.removable,kernel_plan.removed);
    if(removing&&undo_missing)kiln::tools::note_no_undo_record();
    reinterpret_cast<uint8_t*>(call->args[0])[0]=game_code;
    return call->args[0];
}
KilnResult KILN_CALL detour(KilnOwner,uintptr_t,const KilnHookOrder*,uint32_t count,KilnDetourFn2 fn,void*,KilnHandle* out){check(count==3&&fn==&kiln_tools_impact_detour);*out=1;return KILN_OK;}
const void* KILN_CALL service(KilnStr name,uint32_t){check(std::string_view(name.ptr,name.len)=="kiln.hooks");return &hooks;}
KilnResult KILN_CALL symbol(KilnStr name,uintptr_t* out){check(std::string_view(name.ptr,name.len)=="gameplay/TerraformImpact");*out=1;return KILN_OK;}
KilnResult KILN_CALL constant(KilnStr name,int64_t* out){
    const std::string_view n(name.ptr,name.len);
    if(n=="tools/area.access_zones"){*out=0x88;return KILN_OK;}
    if(constants_missing)return KILN_ERR_NOT_FOUND;
    if(n=="world/job.storage"){*out=job_storage;return KILN_OK;}
    if(n=="world.storage"){*out=world_storage;return KILN_OK;}
    check(false);return KILN_ERR_NOT_FOUND;
}
}
int main(){
    auto& trace=kiln_tools_action_trace();
    // One refusal of `exit`, recorded as the trace's state, from a stroke `f` the detour refused or dispatched.
    auto exited=[&](ActionExit exit,auto&& stroke){const auto n=trace.count(exit);stroke();return trace.count(exit)==n+1&&trace.last()==exit;};
    hooks.header={sizeof(hooks),2};hooks.detour2=&detour;hooks.call_original2=&original;
    hooks.subscribe_owner_retiring=&watch;hooks.unsubscribe_owner_retiring=&unwatch;
    KilnApi api{};api.service=&service;api.symbol=&symbol;api.constant=&constant;
    hooks.read_original=&read_original;hooks.code_state=&code_state;
    for(auto state:{KILN_CODE_FOREIGN,KILN_CODE_UNKNOWN}) {
        live_code=state;check(!kiln_tools_impact_install(&api,1,&area));
    }
    live_code=KILN_CODE_ORIGINAL;wrong_original=true;
    check(!kiln_tools_impact_install(&api,1,&area));wrong_original=false;
    original_unreadable=true;check(!kiln_tools_impact_install(&api,1,&area));original_unreadable=false;
    state_unreadable=true;check(!kiln_tools_impact_install(&api,1,&area));state_unreadable=false;
    hooks.read_original=nullptr;check(!kiln_tools_impact_install(&api,1,&area));hooks.read_original=&read_original;
    hooks.code_state=nullptr;check(!kiln_tools_impact_install(&api,1,&area));hooks.code_state=&code_state;
    live_code=KILN_CODE_MANAGED;
    // Without the world layout the binding cannot tell the authoritative world: it never installs.
    constants_missing=true;check(!kiln_tools_impact_install(&api,1,&area));constants_missing=false;
    check(kiln_tools_impact_install(&api,1,&area));check(retiring!=nullptr);
    // No family bound (a player without a tool mod): every impact is vanilla and passes through without its context
    // being read, even one this hook could not read
    {Fixture f;f.call.args[2]=0;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1&&f.result[0]!=0x18);}
    check(!trace.any()&&trace.others()==0&&kiln_tools_bound_programs()==0); // nothing bound: nothing traced either
    KilnToolBrush brush{42,0,2};KilnToolPoolMaterial material{55,8,16,0};KilnToolFamilyDef family{};
    family.tool=99;family.brushes=&brush;family.brush_count=1;family.materials=&material;family.material_count=1;
    family.primary.mode=KILN_TOOL_MODE_REMOVE;family.primary.program=100;family.primary.flags=KILN_TOOL_TERRAIN_ONLY;family.primary.remove_cells[1]=1;
    family.secondary.mode=KILN_TOOL_MODE_ADD;family.secondary.program=101;family.secondary.flags=KILN_TOOL_TERRAIN_ONLY;
    check(kiln_tools_bind_programs(2,family));check(!kiln_tools_bind_programs(3,family));
    check(kiln_tools_bound_programs()==2);
    // With a family bound, a context that cannot be read is refused (its program cannot be known)
    check(exited(ActionExit::Unreadable,[&]{Fixture f;f.call.args[2]=0;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    for(uint32_t i=0;i<KILN_TOOL_HOOK_COUNT;++i)kiln_tools_report_hook(i,1);
    check(kiln_tools_actions_ready(KILN_TOOL_POLICY_REMOVE));check(kiln_tools_actions_ready(KILN_TOOL_POLICY_ADD));
    // Presentation-only hooks are not authority requirements, including on dedicated servers.
    for(auto h:{KILN_TOOL_HOOK_SELECTOR_BUILDER,KILN_TOOL_HOOK_PREVIEW,KILN_TOOL_HOOK_BRUSH_PICKER,KILN_TOOL_HOOK_IDENTITY})kiln_tools_report_hook(h,0);
    check(kiln_tools_actions_ready(KILN_TOOL_POLICY_ADD));
    for(auto h:{KILN_TOOL_HOOK_EDGE_CHECK,KILN_TOOL_HOOK_HISTORY_COST,KILN_TOOL_HOOK_KERNEL_FILTER,KILN_TOOL_HOOK_BUILDING_SUPPRESS}){
        check(exited(ActionExit::NotReady,[&]{Fixture f;kiln_tools_report_hook(h,0);int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);kiln_tools_report_hook(h,1);}));
    }
    // No server world yet: a stroke is not the authoritative world's.
    check(exited(ActionExit::NotServerWorld,[&]{Fixture f;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    kiln_tools_impact_world(server_world);
    for(auto h:{KILN_TOOL_HOOK_COST,KILN_TOOL_HOOK_MATERIAL_LOOKUP,KILN_TOOL_HOOK_EDGE_CHECK,KILN_TOOL_HOOK_HISTORY_COST}) {
        Fixture f;f.ctx.program_id=101;kiln_tools_report_hook(h,0);int before=calls;
        kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);kiln_tools_report_hook(h,1);
    }
    expected_policy=101;expected_type=1;
    // The engine's iterator layout (kiln#133): this stroke reaches the game's terraform, which accepts the Add.
    check(exited(ActionExit::Added,[&]{Fixture f;f.ctx.program_id=101;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1&&!kiln_tools_active_policy());}));
    check(trace.detail().program==101&&trace.detail().item==42&&trace.detail().radius==2&&trace.detail().game==0);
    expected_policy=100;expected_type=0;
    check(!kiln_tools_actions_faulted());
    // Another world's program, a missing program context or iterator: refused before anything is switched.
    check(exited(ActionExit::NotServerWorld,[&]{Fixture f;f.set_world(server_world+0x1000);int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    check(exited(ActionExit::NoAccess,[&]{Fixture f;f.program_context[0]=0;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    check(exited(ActionExit::NoAccess,[&]{Fixture f;f.access[0]=0;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    check(exited(ActionExit::NoAccess,[&]{Fixture f;f.access[0x88/8]=0;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    check(exited(ActionExit::OutsideBase,[&]{Fixture f;allow_area=false;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);allow_area=true;}));
    check(exited(ActionExit::NotFamilyItem,[&]{Fixture f;uint32_t bad=43;std::memcpy(f.data.data()+160,&bad,4);int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    check(trace.detail().item==43);
    // The copy's refusals each have their own exit.
    check(exited(ActionExit::RadiusMismatch,[&]{Fixture f;float r=3;std::memcpy(f.data.data()+144,&r,4);int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    check(trace.detail().item==42&&trace.detail().radius==3);
    check(exited(ActionExit::NoUsedItem,[&]{Fixture f;uint32_t none=0;std::memcpy(f.data.data()+160,&none,4);int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    check(exited(ActionExit::RadiusRange,[&]{Fixture f;float r=9;std::memcpy(f.data.data()+144,&r,4);int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    check(exited(ActionExit::Marker,[&]{Fixture f;f.data[80]=0;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    check(exited(ActionExit::NoActor,[&]{Fixture f;uint32_t none=0;std::memcpy(f.data.data(),&none,4);int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    check(exited(ActionExit::Layout,[&]{Fixture f;f.layout[1].size=8;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    // A Remove stroke the game ran: the terrain kernel's tally names its outcome.
    {Fixture f;int before=calls;auto args=f.call.args[1],ctx=f.call.args[2];
     check(exited(ActionExit::GameStopped,[&]{kiln_tools_impact_detour(&f.call,nullptr);}));
     check(calls==before+1&&f.data[80]==255&&f.call.args[1]==args&&f.call.args[2]==ctx&&!kiln_tools_active_policy());
     check(exited(ActionExit::GameStopped,[&]{kiln_tools_impact_detour(&f.call,nullptr);}));check(calls==before+2);}
    game_code=6;check(exited(ActionExit::GameStopped,[&]{Fixture f;kiln_tools_impact_detour(&f.call,nullptr);check(f.result[0]==6);}));check(trace.detail().game==6);game_code=0;
    undo_missing=true;check(exited(ActionExit::NoUndoRecord,[&]{Fixture f;kiln_tools_impact_detour(&f.call,nullptr);}));undo_missing=false;
    kernel_plan={true,false,0};check(exited(ActionExit::NotRemovable,[&]{Fixture f;kiln_tools_impact_detour(&f.call,nullptr);}));
    kernel_plan={true,true,0};check(exited(ActionExit::NothingMatched,[&]{Fixture f;kiln_tools_impact_detour(&f.call,nullptr);}));
    kernel_plan={true,true,-1};check(exited(ActionExit::Carved,[&]{Fixture f;kiln_tools_impact_detour(&f.call,nullptr);}));
    kernel_plan={true,true,1};check(exited(ActionExit::Removed,[&]{Fixture f;kiln_tools_impact_detour(&f.call,nullptr);}));
    check(kiln::tools::stroke_tally==nullptr); // the stroke's tally is scoped to its continuation
    // A nested vanilla impact inside the stroke is counted as another program; it changes neither the state nor the tally.
    kernel_plan={true,true,0};
    {const auto others=trace.others();check(exited(ActionExit::NothingMatched,[&]{Fixture f;nest=true;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+2&&!kiln_tools_active_policy());}));
     check(trace.others()==others+1);}
    kernel_plan={};
#ifdef _WIN32
    {Fixture f;f.ctx.layout=reinterpret_cast<const uint8_t*>(1);int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18&&!kiln_tools_active_policy());}
#endif
    kiln_tools_unbind_programs(3,99);{Fixture f;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1);}
    kiln_tools_unbind_programs(2,99);expected_policy=0;expected_type=255;{Fixture f;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1&&!kiln_tools_active_policy());}
    check(kiln_tools_bind_programs(2,family));retiring(2,nullptr);
    {Fixture f;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1&&!kiln_tools_active_policy());}
    check(kiln_tools_bind_programs(2,family));expected_policy=100;expected_type=0;retire_in_call=true;
    {Fixture f;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1&&!kiln_tools_active_policy());}
    expected_policy=0;expected_type=255;
    {Fixture f;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1&&!kiln_tools_active_policy());}
    check(kiln_tools_bind_programs(2,family));expected_policy=100;expected_type=0;
    // Closed admission (service shutdown) refuses a bound program; a reinstallation opens it again.
    check(kiln_tools_impact_close());
    check(exited(ActionExit::Closed,[&]{Fixture f;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    check(kiln_tools_impact_install(&api,1,&area));
    check(exited(ActionExit::GameStopped,[&]{Fixture f;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1);}));
    check(exited(ActionExit::ContinuationFault,[&]{Fixture f;fault=true;int before=calls;auto args=f.call.args[1],ctx=f.call.args[2];kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1&&f.result[0]==0x18&&!kiln_tools_active_policy()&&f.call.args[1]==args&&f.call.args[2]==ctx);fault=false;}));
    check(exited(ActionExit::Faulted,[&]{Fixture f;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18&&!kiln_tools_active_policy());}));
    check(kiln_tools_actions_faulted()&&!kiln_tools_actions_ready(KILN_TOOL_POLICY_REMOVE)&&!kiln_tools_actions_ready(KILN_TOOL_POLICY_ADD));
    check(!kiln_tools_impact_install(&api,1,&area));
    {Fixture f;f.ctx.program_id=101;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}
    // The latch disables only tool programs. Vanilla calls and the installed history hooks are not torn down.
    expected_policy=0;expected_type=255;
    {const auto others=trace.others();Fixture f;f.ctx.program_id=999;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1&&!kiln_tools_active_policy()&&trace.others()==others+1);}
    check(kiln_tools_impact_close());
    expected_policy=100;expected_type=0;
    check(exited(ActionExit::Faulted,[&]{Fixture f;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}));
    std::printf("%d impact dispatch checks passed\n",checks);
}
