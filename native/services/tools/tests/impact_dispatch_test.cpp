// SPDX-License-Identifier: MIT
#ifdef _WIN32
#include <windows.h>
#endif
#include "../impact_binding.h"
#include "../impact_args.h"
#include "../internal.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {
int calls=0,checks=0; bool allow_area=true,nest=false,fault=false;
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
KilnResult KILN_CALL watch(KilnOwner,KilnOwnerRetiringFn fn,void*,KilnHandle* out){retiring=fn;*out=2;return KILN_OK;}
KilnResult KILN_CALL unwatch(KilnHandle){return KILN_OK;}
struct Fixture {
    std::array<kab::ImpactVariable,7> layout{};
    std::array<uint8_t,256> data{};
    std::array<uintptr_t,32> access{};
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
        access[0]=0x1234;access[0x88/8]=0x5678;
        ctx.layout=reinterpret_cast<uint8_t*>(layout.data());ctx.layout_count=layout.size();ctx.data=data.data();ctx.data_size=data.size();ctx.iter_ctx=reinterpret_cast<uintptr_t>(access.data());ctx.program_id=100;
        args={handles.data(),9};call.size=sizeof(call);call.arg_count=3;call.args[0]=reinterpret_cast<uintptr_t>(result.data());call.args[1]=reinterpret_cast<uintptr_t>(&args);call.args[2]=reinterpret_cast<uintptr_t>(&ctx);
    }
};
void check(bool b){assert(b);++checks;}
bool area(uintptr_t zones,uint32_t actor,const double p[3],double radius){check(zones==0x5678&&actor==7&&p[0]==0&&radius==2);return allow_area;}
uintptr_t KILN_CALL original(KilnHookCall2* call) {
    ++calls;auto* c=reinterpret_cast<kab::NativeCtx*>(call->args[2]);
    auto* policy=kiln_tools_active_policy();check(expected_policy ? policy&&policy->policy_id==expected_policy : policy==nullptr);
    check(c->data[80]==expected_type);
    if(retire_in_call) {retire_in_call=false;retiring(2,nullptr);check(kiln_tools_active_policy()==policy && policy->pool_count==1 && policy->pool[0].item==55);}
    if(nest){nest=false;Fixture nested;nested.ctx.program_id=999;auto old=expected_policy;auto type=expected_type;expected_policy=0;expected_type=255;kiln_tools_impact_detour(&nested.call,nullptr);expected_policy=old;expected_type=type;check(kiln_tools_active_policy()&&kiln_tools_active_policy()->policy_id==old);}
#ifdef _WIN32
    if(fault)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
#else
    if(fault)throw 7;
#endif
    return call->args[0];
}
KilnResult KILN_CALL detour(KilnOwner,uintptr_t,const KilnHookOrder*,uint32_t count,KilnDetourFn2 fn,void*,KilnHandle* out){check(count==3&&fn==&kiln_tools_impact_detour);*out=1;return KILN_OK;}
const void* KILN_CALL service(KilnStr name,uint32_t){check(std::string_view(name.ptr,name.len)=="kiln.hooks");return &hooks;}
KilnResult KILN_CALL symbol(KilnStr name,uintptr_t* out){check(std::string_view(name.ptr,name.len)=="gameplay/TerraformImpact");*out=1;return KILN_OK;}
KilnResult KILN_CALL constant(KilnStr name,int64_t* out){check(std::string_view(name.ptr,name.len)=="tools/area.access_zones");*out=0x88;return KILN_OK;}
}
int main(){
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
    check(kiln_tools_impact_install(&api,1,&area));check(retiring!=nullptr);
    KilnToolBrush brush{42,0,2};KilnToolPoolMaterial material{55,8,16,0};KilnToolFamilyDef family{};
    family.tool=99;family.brushes=&brush;family.brush_count=1;family.materials=&material;family.material_count=1;
    family.primary.mode=KILN_TOOL_MODE_REMOVE;family.primary.program=100;family.primary.flags=KILN_TOOL_TERRAIN_ONLY;family.primary.remove_cells[1]=1;
    family.secondary.mode=KILN_TOOL_MODE_ADD;family.secondary.program=101;family.secondary.flags=KILN_TOOL_TERRAIN_ONLY;
    check(kiln_tools_bind_programs(2,family));check(!kiln_tools_bind_programs(3,family));
    for(uint32_t i=0;i<KILN_TOOL_HOOK_COUNT;++i)kiln_tools_report_hook(i,1);
    check(kiln_tools_actions_ready(KILN_TOOL_POLICY_REMOVE));check(kiln_tools_actions_ready(KILN_TOOL_POLICY_ADD));
    // Presentation-only hooks are not authority requirements, including on dedicated servers.
    for(auto h:{KILN_TOOL_HOOK_SELECTOR_BUILDER,KILN_TOOL_HOOK_PREVIEW,KILN_TOOL_HOOK_BRUSH_PICKER,KILN_TOOL_HOOK_IDENTITY})kiln_tools_report_hook(h,0);
    check(kiln_tools_actions_ready(KILN_TOOL_POLICY_ADD));
    for(auto h:{KILN_TOOL_HOOK_EDGE_CHECK,KILN_TOOL_HOOK_HISTORY_COST,KILN_TOOL_HOOK_KERNEL_FILTER,KILN_TOOL_HOOK_BUILDING_SUPPRESS}){
        Fixture f;kiln_tools_report_hook(h,0);int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);kiln_tools_report_hook(h,1);
    }
    {Fixture f;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}
    kiln_tools_impact_world(0x1234);
    for(auto h:{KILN_TOOL_HOOK_COST,KILN_TOOL_HOOK_MATERIAL_LOOKUP,KILN_TOOL_HOOK_EDGE_CHECK,KILN_TOOL_HOOK_HISTORY_COST}) {
        Fixture f;f.ctx.program_id=101;kiln_tools_report_hook(h,0);int before=calls;
        kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);kiln_tools_report_hook(h,1);
    }
    expected_policy=101;expected_type=1;
    {Fixture f;f.ctx.program_id=101;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1&&!kiln_tools_active_policy());}
    expected_policy=100;expected_type=0;
    check(!kiln_tools_actions_faulted());
    {Fixture f;f.access[0]=0x1111;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}
    {Fixture f;allow_area=false;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);allow_area=true;}
    {Fixture f;uint32_t bad=43;std::memcpy(f.data.data()+160,&bad,4);int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}
    {Fixture f;int before=calls;auto args=f.call.args[1],ctx=f.call.args[2];kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1&&f.data[80]==255&&f.call.args[1]==args&&f.call.args[2]==ctx&&!kiln_tools_active_policy());kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+2);}
    {Fixture f;nest=true;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+2&&!kiln_tools_active_policy());}
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
    {Fixture f;fault=true;int before=calls;auto args=f.call.args[1],ctx=f.call.args[2];kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1&&f.result[0]==0x18&&!kiln_tools_active_policy()&&f.call.args[1]==args&&f.call.args[2]==ctx);fault=false;}
    {Fixture f;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18&&!kiln_tools_active_policy());}
    check(kiln_tools_actions_faulted()&&!kiln_tools_actions_ready(KILN_TOOL_POLICY_REMOVE)&&!kiln_tools_actions_ready(KILN_TOOL_POLICY_ADD));
    check(!kiln_tools_impact_install(&api,1,&area));
    {Fixture f;f.ctx.program_id=101;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before&&f.result[0]==0x18);}
    // The latch disables only tool programs. Vanilla calls and the installed history hooks are not torn down.
    expected_policy=0;expected_type=255;
    {Fixture f;f.ctx.program_id=999;int before=calls;kiln_tools_impact_detour(&f.call,nullptr);check(calls==before+1&&!kiln_tools_active_policy());}
    check(kiln_tools_impact_close());
    std::printf("%d impact dispatch checks passed\n",checks);
}
