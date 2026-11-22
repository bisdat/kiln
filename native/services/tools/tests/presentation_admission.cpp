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
static bool nested_preview;
static PreviewOwner nested_out;
static PreviewOwner* vanilla_preview(PreviewOwner* out,const uint32_t*,SelectorGroup*) {
    *out={};out->owned.data=out->owned.inline_rows;out->owned.capacity=64;out->owned.count=2;
    out->owned.data[0].item=999;out->owned.data[0].count=9;out->owned.data[1].item=1257792166u;out->owned.data[1].count=3;
    static PreviewRow borrowed[2];out->view=borrowed;out->view_count=2;out->selected_item=999;
    if(nested_preview) {nested_preview=false;uint32_t vanilla=0xbe8b0750;preview(&nested_out,&vanilla,nullptr);}
    return out;
}
static KilnResult KILN_CALL raw_admission(KilnOwner,uintptr_t target,const KilnHookOrder*,void*,void** original,KilnHandle* handle) {
    *original=target==2?reinterpret_cast<void*>(&vanilla_selector):target==3?reinterpret_cast<void*>(&vanilla_preview):reinterpret_cast<void*>(&vanilla_identity);
    *handle=target;return KILN_OK;
}
int main() {
    boundary_fixture();
    fake_hooks.raw_detour=&raw_admission;
    KilnApi fake{};fake.size=sizeof fake;fake.abi_version=KILN_ABI_VERSION;fake.service=&service;fake.subscribe_tick=&tick;fake.unsubscribe=&unsubscribe;fake.owner_revoke=&owner_revoke;
    api=&fake;hooks=&fake_hooks;child=88;retiring=99;
    for(uint32_t i=0;i<KILN_TOOL_HOOK_COUNT;++i)kiln_tools_report_hook(i,1);
    auto family=definition();KilnHandle handle=0;CHECK(kiln_tools_define_family(7,&family,&handle)==KILN_OK);
    CHECK(selector_hook.install(&fake,child,2,reinterpret_cast<void*>(&selector),"selector fixture"));
    CHECK(preview_hook.install(&fake,child,3,reinterpret_cast<void*>(&preview),"preview fixture"));
    kiln_tools_presentation_set_held_provider(&fixture_held_provider);
    SelectorGroup groups[7];held_item=family.tool;selector(groups,nullptr,0);
    CHECK(groups[2].count==1&&groups[2].data[0].item==1257792166u&&!groups[2].has_selected);
    CHECK(groups[2].data[0].count==3); // Aggregate stock is preserved, not re-added once per policy.
    for(unsigned i:{0,1,3,4,5,6})CHECK(groups[i].count==2&&groups[i].has_selected&&groups[i].data[0].count==9);
    held_item=0xbe8b0750;selector(groups,nullptr,0);CHECK(groups[2].count==2&&groups[2].has_selected);
    held_item=family.tool;selector(groups,nullptr,0);CHECK(groups[2].count==1); // Tool switch is read per invocation.
    held_item=0;selector(groups,nullptr,0);CHECK(groups[2].count==2);
    PreviewOwner own;uint32_t own_id=family.tool;held_item=family.tool;nested_preview=true;preview(&own,&own_id,nullptr);
    CHECK(own.view_count==1&&own.view==own.owned.inline_rows&&own.view[0].item==1257792166u);
    CHECK(nested_out.view_count==2); // Unrelated preview never inherits an outer family.
    uint32_t vanilla=0xbe8b0750;preview(&own,&vanilla,nullptr);CHECK(own.view_count==2);
    kiln_tools_presentation_set_held_provider(nullptr);kiln_tools_presentation_uninstall();
    std::cout<<"presentation admission: "<<checks<<" checks passed (includes boundary fixture)\n";
}
