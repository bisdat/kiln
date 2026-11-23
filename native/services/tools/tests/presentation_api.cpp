// SPDX-License-Identifier: MIT
// Native service boundary fixtures. These simulate capability readiness, not game execution.
#include "../hooks/presentation.cpp"
#include <cstdlib>
#include <iostream>
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
static bool bind_ok=true;
bool kiln_tools_bind_programs(KilnOwner,const KilnToolFamilyDef&) noexcept {return bind_ok;}
void kiln_tools_unbind_programs(KilnOwner,uint32_t) noexcept {}
static bool identity_has_policy;
static RootIdentity* vanilla_identity(RootIdentity* out,const uint32_t*) {identity_has_policy=kiln_tools_active_policy()!=nullptr;*out={123,456,789,1,{}};return out;}
static unsigned lookup_calls;
static const uint8_t* KILN_CALL vanilla_lookup(void*,uint8_t) {static uint8_t config; ++lookup_calls;return &config;}
static KilnHooksApi fake_hooks{};
static KilnOwner revoked_owner{};
static bool statuses_closed{};
static KilnCapsApi fake_caps{};
struct Capability {uint32_t state,reason;std::string text;};
static std::map<std::string,Capability> capabilities;
static KilnResult KILN_CALL report_cap(KilnOwner,KilnStr name,uint32_t state,uint32_t reason,KilnStr text) {
    capabilities[std::string(name.ptr,name.len)]={state,reason,std::string(text.ptr,text.len)};return KILN_OK;
}
static const void* KILN_CALL service(KilnStr name,uint32_t) {
    const std::string id(name.ptr,name.len);
    return id==KILN_HOOKS_SERVICE?static_cast<const void*>(&fake_hooks):id==KILN_CAPS_SERVICE?&fake_caps:nullptr;
}
static unsigned retiring_attempts;
static KilnOwner KILN_CALL parent_returned(KilnStr) {return 66;}
static KilnResult KILN_CALL retire_refused(KilnOwner,KilnOwnerRetiringFn,void*,KilnHandle*) {++retiring_attempts;return KILN_ERR_UNAVAILABLE;}
static uint32_t reported_side=KILN_SIDE_CLIENT;
static bool reported_online=true,reported_hooks=true;
static KilnResult KILN_CALL client_build(KilnBuildInfo* out) {
    if(!reported_online)return KILN_ERR_UNAVAILABLE;
    *out={};out->size=sizeof(*out);out->side=reported_side;out->verified=1;out->hooks_enabled=reported_hooks;
    std::strcpy(out->exe_sha256,"5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77");return KILN_OK;
}
static KilnResult KILN_CALL raw(KilnOwner,uintptr_t target,const KilnHookOrder*,void*,void** original,KilnHandle* handle) {*original=target==2?reinterpret_cast<void*>(&vanilla_lookup):reinterpret_cast<void*>(&vanilla_identity);*handle=target;return KILN_OK;}
static KilnResult KILN_CALL revoke(KilnHandle) {return KILN_OK;}
static KilnResult KILN_CALL revoke_owner(KilnOwner owner) {
    revoked_owner=owner;statuses_closed=!kiln_tools_hook_installed(KILN_TOOL_HOOK_IDENTITY)&&
        (!fake_caps.report||capabilities["tools.identity"].state==KILN_CAP_UNAVAILABLE);return KILN_OK;
}
static void KILN_CALL owner_revoke(KilnOwner owner) {revoked_owner=owner;}
static bool teardown_on_lease,retire_on_lease,lease_owner_gone;
static KilnResult KILN_CALL tick(KilnOwner owner,int32_t,KilnTickFn,void*,KilnHandle* out) {
    if(lease_owner_gone)return KILN_ERR_REVOKED;
    if(teardown_on_lease) {teardown_on_lease=false;kiln_tools_presentation_uninstall();}
    if(retire_on_lease) {retire_on_lease=false;owner_retiring(owner,nullptr);lease_owner_gone=true;}
    *out=9;return KILN_OK;
}
static KilnResult KILN_CALL unsubscribe(KilnHandle) {return KILN_OK;}
static uint32_t qualification_state=KILN_CODE_ORIGINAL;
static KilnResult KILN_CALL original_bytes(uintptr_t,void* out,size_t size) {
    static const uint8_t bytes[]={0x55,0x48,0x89,0xe5};if(size>sizeof bytes)return KILN_ERR_INVALID_ARGUMENT;
    std::memcpy(out,bytes,size);return KILN_OK;
}
static KilnResult KILN_CALL code_state(uintptr_t,size_t,uint32_t* state,uint64_t* generation) {*state=qualification_state;*generation=1;return KILN_OK;}
static uint32_t hash_guid(const uint8_t* guid) {return fnv(guid,16);}
static KilnToolFamilyDef definition(uint8_t last=1) {
    static KilnToolPoolMaterial materials[]={{1257792166u,8,16,0}};
    static KilnToolBrush brushes[]={{3155237504u,0,0.5}};
    KilnToolFamilyDef value{};value.size=sizeof value;value.pool=123;
    const uint8_t guid[]={0x9f,0xeb,0x2c,0xc5,0xb0,0xb1,0x4b,0x4b,0xa8,0x01,0x52,0xbf,0x83,0x40,0xc0,last};
    std::memcpy(value.identity,guid,16);value.tool=hash_guid(guid);
    value.primary.mode=value.secondary.mode=KILN_TOOL_MODE_REMOVE;value.primary.program=100+last;value.secondary.program=200+last;
    value.primary.flags=value.secondary.flags=KILN_TOOL_TERRAIN_ONLY;value.primary.remove_cells[1]=value.secondary.remove_cells[1]=1;
    value.name={"Own terrain tool",16};value.description={"Own description",15};value.materials=materials;value.material_count=1;
    value.brushes=brushes;value.brush_count=1;return value;
}
int main() {
    fake_hooks.header={sizeof(fake_hooks),KILN_HOOKS_VERSION};fake_hooks.raw_detour=&raw;fake_hooks.revoke=&revoke;fake_hooks.revoke_owner=&revoke_owner;
    KilnApi fake{};fake.size=sizeof fake;fake.abi_version=KILN_ABI_VERSION;fake.service=&service;fake.subscribe_tick=&tick;fake.unsubscribe=&unsubscribe;fake.owner_revoke=&owner_revoke;
    api=&fake;hooks=&fake_hooks;child=88;retiring=99;
    CHECK(identity_hook.install(&fake,child,1,reinterpret_cast<void*>(&identity),"test identity"));
    for(uint32_t i=0;i<KILN_TOOL_HOOK_COUNT;++i)kiln_tools_report_hook(i,1);
    auto first=definition();CHECK(first.tool==3205570361u);
    KilnHandle one=0;CHECK(kiln_tools_define_family(7,&first,&one)==KILN_OK&&one!=0);
    RootIdentity own{};uint32_t item=first.tool;CHECK(identity(&own,&item)==&own&&own.item==first.tool&&own.present);
    CHECK(own.name!=456&&own.description!=789);
    item=first.brushes[0].item;CHECK(identity(&own,&item)->item==first.tool);
    uint32_t vanilla=0xbe8b0750;CHECK(identity(&own,&vanilla)->item==123&&own.name==456&&own.description==789);
    const auto* policy=kt::policy_for_tool(first.tool,0);CHECK(policy&&policy->pool_count==1&&policy->pool[0].cell==8);
    const auto* brush_policy=kt::policy_for_tool(first.brushes[0].item,0);CHECK(brush_policy&&brush_policy->policy_id==first.primary.program);
    auto second=definition(12);KilnToolBrush second_brush{777,0,1};second.brushes=&second_brush;
    KilnHandle two=0;CHECK(kiln_tools_define_family(7,&second,&two)==KILN_OK);
    CHECK(kiln_tools_remove_family(8,one)==KILN_ERR_INVALID_ARGUMENT);
    CHECK(kiln_tools_remove_family(7,one)==KILN_OK);
    CHECK(!kt::policy_for_tool(first.tool,0)&&families.root(first.tool)==0);
    CHECK(families.root(second.tool)==second.tool); // Removing one family must not erase the owner's other tool.
    owner_retiring(7,nullptr);CHECK(!kt::policy_for_tool(second.tool,0)&&families.root(second.tool)==0);
    first.identity[0]^=1;CHECK(kiln_tools_define_family(7,&first,&one)==KILN_ERR_INVALID_ARGUMENT&&one==0);first.identity[0]^=1;
    bind_ok=false;CHECK(kiln_tools_define_family(7,&first,&one)==KILN_ERR_UNAVAILABLE&&one==0);
    CHECK(!kt::policy_for_tool(first.tool,0)&&families.root(first.tool)==0);bind_ok=true;
    first.primary.mode=KILN_TOOL_MODE_ADD;std::memset(first.primary.remove_cells,0,32);
    kiln_tools_report_hook(KILN_TOOL_HOOK_SELECTOR_BUILDER,0);
    CHECK(kiln_tools_define_family(7,&first,&one)==KILN_OK&&one!=0); // Client presentation never gates server Add.
    CHECK(lookup_hook.install(&fake,child,2,reinterpret_cast<void*>(&lookup),"test material lookup"));
    lookup_calls=0;CHECK(lookup(nullptr,69)!=nullptr&&lookup_calls==1); // Vanilla is unchanged with a policy registered.
    {
        kt::ToolPolicyScope scope(kt::policy_for_tool(first.tool,0));const auto* p=kiln_tools_active_policy();
        CHECK(p&&p->pool_count==1&&p->pool[0].item==1257792166u&&p->pool[0].cell==8&&p->pool[0].voxels_per_item==16);
        CHECK(lookup(nullptr,8)!=nullptr&&lookup_calls==2);
        CHECK(lookup(nullptr,69)==nullptr&&lookup_calls==2); // Lookup, like cost, uses the private immutable pool.
    }
    CHECK(lookup(nullptr,69)!=nullptr&&lookup_calls==3);
    CHECK(kiln_tools_remove_family(7,one)==KILN_OK);
    kiln_tools_report_hook(KILN_TOOL_HOOK_COST,0);
    CHECK(kiln_tools_define_family(7,&first,&one)==KILN_ERR_UNAVAILABLE&&one==0);
    CHECK(!kt::policy_for_tool(first.tool,0));
    KilnToolMaterialSnapshot snapshot;std::memset(&snapshot,0xcc,sizeof snapshot);snapshot.size=sizeof snapshot;
    CHECK(kiln_tools_material_snapshot(1,first.tool,&snapshot)==KILN_ERR_UNAVAILABLE);
    CHECK(snapshot.size==sizeof snapshot&&snapshot.count==0&&snapshot.selected_item==0&&snapshot.session==0);
    CHECK(std::all_of(reinterpret_cast<const uint8_t*>(snapshot.rows),reinterpret_cast<const uint8_t*>(snapshot.rows)+sizeof snapshot.rows,[](uint8_t b){return b==0;}));
    fake.build_info=&client_build;parent_owner=66;fake_caps.report=&report_cap;report_caps();
    CHECK(capabilities["tools.identity"].state==KILN_CAP_AVAILABLE);
    kiln_tools_presentation_uninstall();CHECK(revoked_owner==88&&statuses_closed);
    CHECK(capabilities["tools.identity"].state==KILN_CAP_UNAVAILABLE);
    fake.owner_create=&parent_returned;fake.build_info=&client_build;fake_hooks.subscribe_owner_retiring=&retire_refused;
    revoked_owner=0;retiring_attempts=0;kiln_tools_presentation_install(&fake,66);
    CHECK(child==0&&revoked_owner==0&&retiring_attempts==0); // Never revoke the parent when child creation aliases it.
    KilnToolPresentationStatus status{};status.size=sizeof status;
    reported_online=false;CHECK(kiln_tools_presentation_status(&status)==KILN_OK&&status.reason==KILN_CAP_REASON_OFFLINE);
    reported_online=true;reported_side=KILN_SIDE_SERVER;
    CHECK(kiln_tools_presentation_status(&status)==KILN_OK&&status.reason==KILN_CAP_REASON_WRONG_SIDE);
    kiln_tools_report_hook(KILN_TOOL_HOOK_MATERIAL_LOOKUP,1);report_caps();
    CHECK(capabilities.contains("tools.material_lookup")&&capabilities["tools.material_lookup"].state==KILN_CAP_AVAILABLE);
    kiln_tools_report_hook(KILN_TOOL_HOOK_MATERIAL_LOOKUP,0);report_caps();
    CHECK(capabilities["tools.material_lookup"].state==KILN_CAP_UNAVAILABLE);
    reported_side=KILN_SIDE_CLIENT;reported_hooks=false;
    CHECK(kiln_tools_presentation_status(&status)==KILN_OK&&status.reason==KILN_CAP_REASON_POLICY);
    reported_hooks=true;report_caps();fake_caps.report=nullptr;
    api=&fake;hooks=&fake_hooks;child=88;retiring=99;
    for(uint32_t i=0;i<KILN_TOOL_HOOK_COUNT;++i)kiln_tools_report_hook(i,1);
    auto racing=definition();teardown_on_lease=true;
    CHECK(kiln_tools_define_family(7,&racing,&one)==KILN_ERR_UNAVAILABLE&&one==0);
    CHECK(!families.family(racing.tool)&&!kt::policy_for_tool(racing.tool,0));
    child=88;retiring=99;
    for(uint32_t i=0;i<KILN_TOOL_HOOK_COUNT;++i)kiln_tools_report_hook(i,1);
    retire_on_lease=true;
    CHECK(kiln_tools_define_family(7,&racing,&one)==KILN_ERR_REVOKED&&one==0);
    CHECK(!families.family(racing.tool)&&!kt::policy_for_tool(racing.tool,0));
    lease_owner_gone=false;kiln_tools_presentation_uninstall();
    fake_hooks.read_original=&original_bytes;fake_hooks.code_state=&code_state;
    qualification_state=KILN_CODE_ORIGINAL;CHECK(qualify(1,"55 48 ?? E5"));
    qualification_state=KILN_CODE_MANAGED;CHECK(qualify(1,"55 48 ?? E5"));
    qualification_state=KILN_CODE_FOREIGN;CHECK(!qualify(1,"55 48 ?? E5"));
    qualification_state=KILN_CODE_UNKNOWN;CHECK(!qualify(1,"55 48 ?? E5"));
    qualification_state=KILN_CODE_ORIGINAL;CHECK(!qualify(1,"55 48 88 E5"));
    fake_hooks.read_original=nullptr;CHECK(!qualify(1,"55 48 ?? E5"));
    std::cout<<"presentation API: "<<checks<<" checks passed\n";
    return 0;
}
