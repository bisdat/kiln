// SPDX-License-Identifier: MIT
#include "kiln_api.h"
#include "services/kiln_caps.h"
#include "services/kiln_tools.h"
#include "bootstrap.h"
#include "capabilities.h"
#include "impact_binding.h"
#include "include/kiln_tools_policy.h"
#include "hooks/build_area.h"
#include "hooks/terrain_carve.h"
#include "hooks/terrain_payment.h"
#include "history.h"
#include <atomic>
#include <cstring>

// Hook packages disable only their own links, after the service has closed admission and drained policy scopes.
void kiln_tools_carve_uninstall();
extern "C" void kiln_tools_payment_uninstall();

#if __has_include("presentation_api.h")
#include "presentation_api.h"
#define KILN_TOOLS_PRESENTATION 1
#else
#define KILN_TOOLS_PRESENTATION 0
// A build without the presentation package advertises no family publication, never an unrestricted fallback.
static KilnResult KILN_CALL kiln_tools_define_family(KilnOwner,const KilnToolFamilyDef*,KilnHandle* out) {
    if(out)*out=0;return KILN_ERR_UNAVAILABLE;
}
static KilnResult KILN_CALL kiln_tools_remove_family(KilnOwner,KilnHandle) {return KILN_ERR_NOT_FOUND;}
static KilnResult KILN_CALL kiln_tools_material_snapshot(KilnEntity,uint32_t,KilnToolMaterialSnapshot* out) {
    if(!out || out->size<sizeof(*out))return KILN_ERR_INVALID_ARGUMENT;
    *out={};out->size=sizeof(*out);return KILN_ERR_UNAVAILABLE;
}
static KilnResult KILN_CALL kiln_tools_presentation_status(KilnToolPresentationStatus* out) {
    if(!out || out->size<sizeof(*out))return KILN_ERR_INVALID_ARGUMENT;
    *out={};out->size=sizeof(*out);out->reason=KILN_CAP_REASON_SERVICE_MISSING;return KILN_OK;
}
#endif

namespace {
const KilnApi* api=nullptr;
const KilnCapsApi* caps=nullptr;
KilnOwner service_owner=0;
KilnHandle tick_subscription=0,world_subscription=0;
std::atomic<uintptr_t> world{};
std::atomic<bool> lifecycle{},impact_ready{};

template<size_t N> constexpr KilnStr str(const char(&s)[N]) {return {s,N-1};}
KilnStr text(const char* s) {return {s,std::strlen(s)};}
void report(const char* name,uint32_t state,uint32_t reason,const char* message) {
    if(caps)caps->report(service_owner,text(name),state,reason,text(message));
}
uint32_t session_state() {
    KilnSessionInfo s{};s.size=sizeof(s);
    return api && api->session && api->session(&s)==KILN_OK ? s.state : KILN_SESSION_NONE;
}
kt::ActionAvailability availability(uint32_t mode) {
    return kt::action_availability(api!=nullptr,lifecycle.load() && kiln_tools_actions_ready(mode),
        session_state(),world.load()!=0,kiln_tools_actions_faulted());
}
void report_caps() {
    const auto remove=availability(KILN_TOOL_POLICY_REMOVE),add=availability(KILN_TOOL_POLICY_ADD);
    report("tools.actions.remove",remove.state,remove.reason,remove.why);
    report("tools.actions.add",add.state,add.reason,add.why);
    const auto any=remove.state==KILN_CAP_AVAILABLE ? remove : add.state==KILN_CAP_AVAILABLE ? add :
        remove.state==KILN_CAP_PENDING ? remove : add.state==KILN_CAP_PENDING ? add : remove;
    report("tools.actions",any.state,any.reason,any.why);
    report("tools.callbacks",KILN_CAP_UNAVAILABLE,KILN_CAP_REASON_POLICY,"declarative tool actions only in this release");
    // tools.build_area is also a static sigdb feature; do not mark that static name pending for a missing session.
    const bool area=impact_ready.load() && kiln_tools_hook_installed(KILN_TOOL_HOOK_EDGE_CHECK);
    report("tools.build_area",area?KILN_CAP_AVAILABLE:KILN_CAP_UNAVAILABLE,
        area?KILN_CAP_REASON_NONE:KILN_CAP_REASON_SYMBOL_MISSING,
        area?"":"whole-brush building-area mapping or impact binding is not installed");
}
bool install_impact(const KilnApi* a,KilnOwner owner) {
    const bool on=kiln_tools_impact_install(a,owner,&kiln_tools_check_build_area);
    impact_ready.store(on);return on;
}
bool install_area(const KilnApi* a,KilnOwner) {return kiln_tools_build_area_init(a);}
void stop_area() {kiln_tools_report_hook(KILN_TOOL_HOOK_EDGE_CHECK,0);}
bool install_carve(const KilnApi* a,KilnOwner owner) {
    kiln_tools_carve_install(a,owner);
    return kiln_tools_hook_installed(KILN_TOOL_HOOK_KERNEL_FILTER) && kiln_tools_hook_installed(KILN_TOOL_HOOK_BUILDING_SUPPRESS);
}
bool install_payment(const KilnApi* a,KilnOwner owner) {return kiln_tools_payment_install(a,owner);}
bool install_history(const KilnApi* a,KilnOwner owner) {
    const bool on=kiln_tools_history_install(a,owner);
    kiln_tools_report_hook(KILN_TOOL_HOOK_HISTORY_COST,on);return on;
}
bool install_presentation(const KilnApi* a,KilnOwner owner) {
#if KILN_TOOLS_PRESENTATION
    kiln_tools_presentation_install(a,owner);
    return true; // individual authority/presentation hook bits, not this return value, govern each action
#else
    (void)a;(void)owner;return false;
#endif
}
void stop_presentation() {
#if KILN_TOOLS_PRESENTATION
    kiln_tools_presentation_uninstall();
#endif
}
// One explicit order. No install runs before every service DLL has loaded; history discovers the loaded rules bridge.
const kt::ToolPackage packages[]={
    {&install_impact,&kiln_tools_impact_uninstall},
    {&install_area,&stop_area},
    {&install_carve,&kiln_tools_carve_uninstall},
    {&install_payment,&kiln_tools_payment_uninstall},
    {&install_history,&kiln_tools_history_uninstall},
    {&install_presentation,&stop_presentation},
};
void KILN_CALL pre(const KilnTickInfo*,void*) {
    const auto w=lifecycle.load() && api->server_world ? api->server_world() : 0;
    world.store(w);kiln_tools_impact_world(w);report_caps();
}
void KILN_CALL changed(const KilnWorldEvent* event,void*) {
    if(event && event->kind==KILN_WORLD_SERVER) {
        // STARTED is not a license to use the world before its first safe Pre. STOPPING invalidates admission now.
        world.store(0);kiln_tools_impact_world(0);
    }
    report_caps();
}
void KILN_CALL ready(void*) {
    caps=static_cast<const KilnCapsApi*>(api->service(str(KILN_CAPS_SERVICE),KILN_CAPS_VERSION));
    const bool life=api->feature(str("world.lifecycle")) &&
        api->subscribe_world(service_owner,&changed,nullptr,&world_subscription)==KILN_OK &&
        api->subscribe_tick(service_owner,KILN_TICK_PRE,&pre,nullptr,&tick_subscription)==KILN_OK;
    lifecycle.store(life);
    kt::install_packages(api,service_owner,packages);
    report_caps();
}
void KILN_CALL shutdown(void*) {
    // The game has returned and native world/undo jobs are quiesced. The DLL/continuations stay resident forever.
    if(!kt::uninstall_packages(packages,&kiln_tools_impact_close))return;
    lifecycle.store(false);impact_ready.store(false);world.store(0);kiln_tools_impact_world(0);
    if(tick_subscription)api->unsubscribe(tick_subscription);
    if(world_subscription)api->unsubscribe(world_subscription);
    tick_subscription=world_subscription=0;
}
KilnResult KILN_CALL status(KilnToolsStatus* out) {
    if(!out || out->size<sizeof(KilnToolsStatus))return KILN_ERR_INVALID_ARGUMENT;
    *out={};out->size=sizeof(*out);
    out->actions=lifecycle.load() && (kiln_tools_actions_ready(KILN_TOOL_POLICY_REMOVE) || kiln_tools_actions_ready(KILN_TOOL_POLICY_ADD));
    out->session=world.load()!=0;
    const auto a=availability(KILN_TOOL_POLICY_REMOVE),b=availability(KILN_TOOL_POLICY_ADD);
    out->reason=a.state==KILN_CAP_AVAILABLE?0:b.state==KILN_CAP_AVAILABLE?0:a.reason;
    out->dispatched=kiln_tools_actions_dispatched();out->refused=kiln_tools_actions_refused();
    return KILN_OK;
}
KilnResult KILN_CALL register_action(KilnOwner,const KilnToolActionDef*,KilnHandle* out) {
    if(out)*out=0;return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL unregister_action(KilnOwner,KilnHandle) {return KILN_ERR_NOT_FOUND;}
const KilnToolsApi table={{sizeof(KilnToolsApi),KILN_TOOLS_VERSION},&status,&register_action,&unregister_action,
    &kiln_tools_define_family,&kiln_tools_remove_family,&kiln_tools_material_snapshot,&kiln_tools_presentation_status};
}
extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* host,KilnOwner owner,KilnPluginInfo* info) {
    if(!host || host->abi_version!=KILN_ABI_VERSION || host->size<sizeof(KilnApi) || !info)return KILN_ERR_UNAVAILABLE;
    api=host;service_owner=owner;
    info->id=str(KILN_TOOLS_SERVICE);info->version=str("1");info->on_ready=&ready;info->on_unload=&shutdown;
    const auto result=api->register_service(owner,str(KILN_TOOLS_SERVICE),&table);
#if KILN_TOOLS_PRESENTATION
    if(result==KILN_OK)return api->register_service(owner,str(KILN_UI_AMMO_SERVICE),kiln_tools_ammo_api());
#endif
    return result;
}
