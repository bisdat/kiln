// SPDX-License-Identifier: MIT
#define main scope_boundary_fixture
#include "presentation_api.cpp"
#undef main
static bool inherited_policy;
static void KILN_CALL scope_original(SelectorGroup*,void*,void*,void*,void*,void*) {inherited_policy=kiln_tools_active_policy()!=nullptr;}
static KilnResult KILN_CALL scope_raw(KilnOwner,uintptr_t target,const KilnHookOrder*,void*,void** original,KilnHandle* handle) {
    *original=reinterpret_cast<void*>(&scope_original);*handle=target;return KILN_OK;
}
int main() {
    fake_hooks.header={sizeof(fake_hooks),KILN_HOOKS_VERSION};fake_hooks.raw_detour=&scope_raw;
    KilnApi host{};host.size=sizeof host;host.service=&service;
    CHECK(ammo::finalizer_hook.install(&host,88,1,reinterpret_cast<void*>(&ammo::finalizer),"ammo scope fixture"));
    KilnToolPolicyView add{};add.size=sizeof add;add.mode=KILN_TOOL_POLICY_ADD;
    {
        kt::ToolPolicyScope outer(add);const auto* pinned=kiln_tools_active_policy();CHECK(pinned);
        ammo::finalizer(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr);
        CHECK(!inherited_policy&&kiln_tools_active_policy()==pinned);
    }
    std::cout<<"ammo finalizer scope: "<<checks<<" checks passed\n";
}
