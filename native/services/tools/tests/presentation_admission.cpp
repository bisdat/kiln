// SPDX-License-Identifier: MIT
#define main boundary_fixture
#include "presentation_api.cpp"
#undef main
static uint32_t held_item;
static uint32_t KILN_CALL fixture_held_provider() {return held_item;}
static void vanilla_selector(SelectorGroup* groups,void*,uint8_t) {
    for(unsigned i=0;i<7;++i) {
        groups[i]={};groups[i].data=groups[i].inline_rows;groups[i].capacity=64;groups[i].count=2;
        groups[i].data[0].item=999;groups[i].data[0].count=9;groups[i].data[1].item=1257792166u;groups[i].data[1].count=3;
        groups[i].has_selected=1;groups[i].selected=0;
    }
}
static bool original_has_policy,inaccessible_rows;
static void policy_selector(SelectorGroup* groups,void* access,uint8_t only) {
    original_has_policy=kiln_tools_active_policy()!=nullptr;vanilla_selector(groups,access,only);
    if(inaccessible_rows)groups[2].data=reinterpret_cast<SelectorRow*>(1);
}
static bool nested_preview;
static PreviewOwner nested_out;
static PreviewOwner* vanilla_preview(PreviewOwner* out,const uint32_t*,SelectorGroup*) {
    original_has_policy=kiln_tools_active_policy()!=nullptr;
    *out={};out->owned.data=out->owned.inline_rows;out->owned.capacity=64;out->owned.count=2;
    out->owned.data[0].item=999;out->owned.data[0].count=9;out->owned.data[1].item=1257792166u;out->owned.data[1].count=3;
    static PreviewRow borrowed[2];out->view=borrowed;out->view_count=2;out->selected_item=999;
    if(nested_preview) {nested_preview=false;uint32_t vanilla=0xbe8b0750;preview(&nested_out,&vanilla,nullptr);}
    if(inaccessible_rows)out->owned.data=reinterpret_cast<PreviewRow*>(1);
    return out;
}
#ifdef _WIN32
static bool probe_preview(PreviewOwner* out,const uint32_t* item) {
    __try {preview(out,item,nullptr);return true;} __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
static bool probe_identity(RootIdentity* out,const uint32_t* item) {
    __try {identity(out,item);return true;} __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
static bool probe_selector(SelectorGroup* out,void* access) {
    __try {selector(out,access,0);return true;} __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
#endif
static KilnResult KILN_CALL raw_admission(KilnOwner,uintptr_t target,const KilnHookOrder*,void*,void** original,KilnHandle* handle) {
    *original=target==2?reinterpret_cast<void*>(&policy_selector):target==3?reinterpret_cast<void*>(&vanilla_preview):reinterpret_cast<void*>(&vanilla_identity);
    *handle=target;return KILN_OK;
}
int main() {
    boundary_fixture();
    fake_hooks.raw_detour=&raw_admission;
    KilnApi fake{};fake.size=sizeof fake;fake.abi_version=KILN_ABI_VERSION;fake.service=&service;fake.subscribe_tick=&tick;fake.unsubscribe=&unsubscribe;fake.owner_revoke=&owner_revoke;
    api=&fake;hooks=&fake_hooks;child=88;retiring=99;
    for(uint32_t i=0;i<KILN_TOOL_HOOK_COUNT;++i)kiln_tools_report_hook(i,1);
    auto family=definition();KilnHandle handle=0;CHECK(kiln_tools_define_family(7,&family,&handle)==KILN_OK);
    selector_game=&policy_selector;
    CHECK(preview_hook.install(&fake,child,3,reinterpret_cast<void*>(&preview),"preview fixture"));
    kiln_tools_presentation_set_held_provider(&fixture_held_provider);
    uint32_t current_item=family.tool;const uint32_t* access[16]{};access[15]=&current_item;
    SelectorGroup groups[7];held_item=family.tool;selector(groups,access,0);
    CHECK(groups[2].count==1&&groups[2].data[0].item==1257792166u&&!groups[2].has_selected);
    CHECK(groups[2].data[0].count==3); // Aggregate stock is preserved, not re-added once per policy.
    for(unsigned i:{0,1,3,4,5,6})CHECK(groups[i].count==2&&groups[i].has_selected&&groups[i].data[0].count==9);
    held_item=0xbe8b0750;selector(groups,access,0);CHECK(groups[2].count==2&&groups[2].has_selected);
    held_item=family.tool;selector(groups,access,0);CHECK(groups[2].count==1); // Tool switch is read per invocation.
    held_item=0;selector(groups,access,0);CHECK(groups[2].count==2);
    PreviewOwner own;uint32_t own_id=family.tool;held_item=family.tool;nested_preview=true;preview(&own,&own_id,nullptr);
    CHECK(own.view_count==1&&own.view==own.owned.inline_rows&&own.view[0].item==1257792166u);
    CHECK(nested_out.view_count==2); // Unrelated preview never inherits an outer family.
    uint32_t vanilla=0xbe8b0750;preview(&own,&vanilla,nullptr);CHECK(own.view_count==2);
    // A prior Post observation is not authority over this invocation's newly held vanilla item.
    held_item=family.tool;current_item=vanilla;selector(groups,access,0);
    CHECK(groups[2].count==2&&groups[2].has_selected&&groups[2].data[0].count==9);
    access[15]=nullptr;selector(groups,access,0);CHECK(groups[2].count==2);
    selector(groups,nullptr,0);CHECK(groups[2].count==2);
    access[15]=&current_item;
    KilnToolPolicyView add{};add.size=sizeof add;add.mode=KILN_TOOL_POLICY_ADD;
    {
        kt::ToolPolicyScope outer(add);const auto* pinned=kiln_tools_active_policy();CHECK(pinned);
        held_item=0;selector(groups,access,0);CHECK(!original_has_policy&&kiln_tools_active_policy()==pinned);
        preview(&own,&vanilla,nullptr);CHECK(!original_has_policy&&kiln_tools_active_policy()==pinned);
        RootIdentity result{};identity(&result,&vanilla);CHECK(!identity_has_policy&&kiln_tools_active_policy()==pinned);
    }
#ifdef _WIN32
    const auto* unreadable=reinterpret_cast<const uint32_t*>(1);
    const bool input_preview=probe_preview(&own,unreadable);
    RootIdentity output{};const bool input_identity=probe_identity(&output,unreadable)&&output.item==123;
    held_item=family.tool;current_item=family.tool;
    CHECK(probe_selector(groups,reinterpret_cast<void*>(1))&&groups[2].count==2);
    access[15]=unreadable;CHECK(probe_selector(groups,access)&&groups[2].count==2);
    access[15]=&current_item;inaccessible_rows=true;
    const bool rows_selector=probe_selector(groups,access)&&groups[2].count==0;
    const bool rows_preview=probe_preview(&own,&own_id)&&own.view_count==0&&own.view==nullptr;
    inaccessible_rows=false;
    std::cout<<"guarded inputs/rows: "<<input_preview<<","<<input_identity<<","<<rows_selector<<","<<rows_preview<<"\n";
    CHECK(input_preview);CHECK(input_identity);CHECK(rows_selector);CHECK(rows_preview);
#endif
    kiln_tools_presentation_set_held_provider(nullptr);kiln_tools_presentation_uninstall();
    std::cout<<"presentation admission: "<<checks<<" checks passed (includes boundary fixture)\n";
}
