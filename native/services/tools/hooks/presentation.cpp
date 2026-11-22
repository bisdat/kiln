// SPDX-License-Identifier: MIT
#include "../presentation_api.h"
#include "../presentation.h"
#include "../held_observer.h"
#include "../internal.h"
#include "../impact_binding.h"
#include "kiln_hook.hpp"
#include "services/kiln_caps.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>

namespace {
using namespace kiln::tools;
const KilnApi* api{};
const KilnHooksApi* hooks{};
KilnOwner child{}, parent_owner{};
KilnHandle retiring{};
Registry families;
std::mutex gate;
struct Registration { KilnOwner owner; uint32_t tool; std::vector<uint32_t> ids; };
std::map<KilnHandle,Registration> registrations;
std::map<uint32_t,std::array<uint8_t,16>> identities;
KilnHandle next_handle=1;
kiln::RawHook lookup_hook, preview_hook, identity_hook, selector_hook;
HeldObserver held_observer;
std::atomic<KilnToolHeldItemProvider> held_provider{};
std::shared_ptr<const Family> held_family() {
    const auto provider=held_provider.load(std::memory_order_acquire);
    const auto item=provider?provider():held_observer.current();
    return families.family(families.root(item));
}
uint32_t fnv(const uint8_t* bytes,size_t length) {uint32_t h=0x811c9dc5u;for(size_t i=0;i<length;++i)h=(h^bytes[i])*0x01000193u;return h;}
uint32_t text_tag(const std::array<uint8_t,16>& identity,const char* purpose) {
    auto guid=identity;const auto hash=fnv(reinterpret_cast<const uint8_t*>(purpose),std::strlen(purpose));
    for(unsigned i=0;i<4;++i)guid[i]^=uint8_t(hash>>(i*8));guid[15]^=0x4b;return fnv(guid.data(),guid.size());
}
bool required_hooks(const KilnToolFamilyAction& action) {
    if(action.mode==KILN_TOOL_MODE_NONE)return true;
    if(!kiln_tools_hook_installed(KILN_TOOL_HOOK_EDGE_CHECK))return false;
    if(action.mode==KILN_TOOL_MODE_REMOVE)return kiln_tools_hook_installed(KILN_TOOL_HOOK_KERNEL_FILTER)&&kiln_tools_hook_installed(KILN_TOOL_HOOK_BUILDING_SUPPRESS);
    return action.mode==KILN_TOOL_MODE_ADD&&kiln_tools_hook_installed(KILN_TOOL_HOOK_COST)&&kiln_tools_hook_installed(KILN_TOOL_HOOK_MATERIAL_LOOKUP)&&
        kiln_tools_hook_installed(KILN_TOOL_HOOK_HISTORY_COST);
}
bool valid_action(const KilnToolFamilyAction& action) {
    if(action.mode>KILN_TOOL_MODE_ADD||action.reserved||action.flags!=KILN_TOOL_TERRAIN_ONLY||!action.program)return false;
    if(action.remove_cells[0]&1)return false;
    for(unsigned i=16;i<32;++i)if(action.remove_cells[i])return false;
    if(action.mode!=KILN_TOOL_MODE_REMOVE)for(auto cell:action.remove_cells)if(cell)return false;
    return true;
}
void remove_locked(const Registration& value) {
    kiln_tools_unbind_programs(value.owner,value.tool); // close admission before removing policy snapshots
    for(auto item:value.ids)kt::unbind_tool(item);
    families.remove(value.owner,value.tool);
    identities.erase(value.tool);
}
void KILN_CALL owner_retiring(KilnOwner owner,void*) {
    std::lock_guard lock(gate);
    for(auto i=registrations.begin();i!=registrations.end();) {
        if(i->second.owner!=owner){++i;continue;}
        remove_locked(i->second);i=registrations.erase(i);
    }
}
void KILN_CALL noop_tick(const KilnTickInfo*,void*) {}

const uint8_t* KILN_CALL lookup(void* registry,uint8_t cell) {
    const auto* policy=kiln_tools_active_policy();
    if(policy&&policy->mode==KILN_TOOL_POLICY_ADD) {
        bool allowed=false;
        if(policy->pool_count<=64&&policy->pool)for(uint32_t i=0;i<policy->pool_count;++i)if(policy->pool[i].cell==cell){allowed=true;break;}
        if(!allowed)return nullptr;
    }
    return lookup_hook.call<const uint8_t*>(registry,cell);
}
void KILN_CALL selector(SelectorGroup* groups,void* access,uint8_t current_only) {
    // A quiet-tick owned observation, sampled once per entry. Never borrow another preview's identity or policy TLS.
    const auto family=held_family();
    selector_hook.call<void>(groups,access,current_only);
    // Blueprint category10 + BuildTool3 + vanilla TerrainMaterials selects group2 before Overgrowth flags.
    // The other six groups (arrows, spells, wall, roof, vanilla Overgrowth, bait) remain untouched.
    if(groups&&family)filter_selector(groups[2],family->materials);
}
PreviewOwner* KILN_CALL preview(PreviewOwner* out,const uint32_t* item,SelectorGroup* group) {
    // Capture this invocation's explicit item identity before the original runs. A nested item-presentation
    // constructor cannot lend its family to this call; only the owned immutable metadata stays pinned.
    auto family=item?families.family(families.root(*item)):nullptr;
    auto* result=preview_hook.call<PreviewOwner*>(out,item,group);
    if(result&&family)filter_preview(*result,family->materials);
    return result;
}
struct RootIdentity {uint32_t item,name,description;uint8_t present,reserved[3];};
static_assert(sizeof(RootIdentity)==16);
RootIdentity* KILN_CALL identity(RootIdentity* out,const uint32_t* item) {
    if(out&&item) {
        const auto root=families.root(*item);
        if(root) {
            std::array<uint8_t,16> guid{};
            {std::lock_guard lock(gate);auto i=identities.find(root);if(i!=identities.end())guid=i->second;}
            if(fnv(guid.data(),guid.size())==root) {*out={root,text_tag(guid,"kiln.item.name"),text_tag(guid,"kiln.item.description"),1,{}};return out;}
        }
    }
    return identity_hook.call<RootIdentity*>(out,item);
}
// Compare verified original bytes, not live detour bytes. Managed chains are valid on reinstall.
bool qualify(uintptr_t address,const char* pattern) {
    if(!hooks||!hooks->read_original||!hooks->code_state)return false;
    std::vector<int> bytes;
    for(const char* p=pattern;*p;) {
        while(*p==' ')++p;if(!*p)break;
        if(p[0]=='?'&&p[1]=='?'){bytes.push_back(-1);p+=2;continue;}
        auto hex=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:-1;};
        if(!p[1]||hex(p[0])<0||hex(p[1])<0)return false;bytes.push_back(hex(p[0])*16+hex(p[1]));p+=2;
    }
    std::vector<uint8_t> original(bytes.size());uint32_t state=KILN_CODE_UNKNOWN;uint64_t generation=0;
    if(bytes.empty()||hooks->read_original(address,original.data(),original.size())!=KILN_OK||hooks->code_state(address,original.size(),&state,&generation)!=KILN_OK||
        (state!=KILN_CODE_ORIGINAL&&state!=KILN_CODE_MANAGED))return false;
    for(size_t i=0;i<bytes.size();++i)if(bytes[i]>=0&&original[i]!=bytes[i])return false;
    return true;
}
void report_caps() {
    if(!api||!api->service||!parent_owner)return;
    const auto* caps=static_cast<const KilnCapsApi*>(api->service({KILN_CAPS_SERVICE,sizeof(KILN_CAPS_SERVICE)-1},KILN_CAPS_VERSION));
    if(!caps||!caps->report)return;
    KilnBuildInfo build{};build.size=sizeof build;
    const bool online=api->build_info&&api->build_info(&build)==KILN_OK;
    const uint32_t reason=!online?KILN_CAP_REASON_OFFLINE:build.side!=KILN_SIDE_CLIENT?KILN_CAP_REASON_WRONG_SIDE:!build.hooks_enabled?KILN_CAP_REASON_POLICY:KILN_CAP_REASON_SYMBOL_MISSING;
    auto report=[&](const char* name,bool ready,const char* missing) {
        const char* why=ready?"qualified explicit-item presentation adapter installed":!online?"no game in this process":build.side!=KILN_SIDE_CLIENT?"tool presentation is client-only":!build.hooks_enabled?"hooks are disabled by the build policy":missing;
        caps->report(parent_owner,{name,std::strlen(name)},ready?KILN_CAP_AVAILABLE:KILN_CAP_UNAVAILABLE,ready?KILN_CAP_REASON_NONE:reason,{why,std::strlen(why)});
    };
    report("tools.material_picker",online&&build.side==KILN_SIDE_CLIENT&&kiln_tools_hook_installed(KILN_TOOL_HOOK_SELECTOR_BUILDER),"selector or owned held-tool observer is unavailable");
    report("tools.material_snapshot",false,"separate per-player backpack/magic-chest observations are not qualified; aggregate selector counts are not split or fabricated");
    report("tools.preview",online&&build.side==KILN_SIDE_CLIENT&&kiln_tools_hook_installed(KILN_TOOL_HOOK_PREVIEW),"owned preview constructor mapping is unavailable or its hook was refused");
    report("tools.brush_families",false,"brush query owning-family admission is not qualified; presentation is refused");
    report("tools.identity",online&&build.side==KILN_SIDE_CLIENT&&kiln_tools_hook_installed(KILN_TOOL_HOOK_IDENTITY),"root identity mapping is unavailable or its hook was refused");
}
bool install(kiln::RawHook& hook,const char* symbol,const char* pattern,void* function) {
    uintptr_t address=0;
    return api->symbol({symbol,std::strlen(symbol)},&address)==KILN_OK&&address&&qualify(address,pattern)&&hook.install(api,child,address,function,symbol);
}
}

void kiln_tools_presentation_set_held_provider(KilnToolHeldItemProvider provider) {
    held_provider.store(provider,std::memory_order_release);
}
KilnResult KILN_CALL kiln_tools_define_family(KilnOwner owner,const KilnToolFamilyDef* definition,KilnHandle* out) {
    if(out)*out=0;
    if(!definition||!out||!owner||definition->size<sizeof(KilnToolFamilyDef)||!definition->tool||definition->reserved||definition->reserved2||definition->reserved3||
        definition->material_count>64||definition->brush_count<1||definition->brush_count>64||(!definition->materials&&definition->material_count)||!definition->brushes||
        !definition->name.ptr||definition->name.len<1||definition->name.len>1024||definition->description.len>16384||(!definition->description.ptr&&definition->description.len)||
        !valid_action(definition->primary)||!valid_action(definition->secondary)||fnv(definition->identity,16)!=definition->tool)return KILN_ERR_INVALID_ARGUMENT;
    if(!api||!hooks||!child||!retiring||!required_hooks(definition->primary)||!required_hooks(definition->secondary))return KILN_ERR_UNAVAILABLE;
    KilnHandle lease=0;auto alive=api->subscribe_tick(owner,KILN_TICK_PRE,&noop_tick,nullptr,&lease);if(alive!=KILN_OK)return alive;api->unsubscribe(lease);
    try {
        Family family{definition->tool,std::string(definition->name.ptr,definition->name.len),std::string(definition->description.ptr?definition->description.ptr:"",definition->description.len),{}, {}};
        std::vector<KilnToolPoolEntry> pool;pool.reserve(definition->material_count);
        for(uint32_t i=0;i<definition->material_count;++i) {
            const auto& entry=definition->materials[i];if(entry.reserved)return KILN_ERR_INVALID_ARGUMENT;
            family.materials.push_back({entry.item,entry.terrain_material,entry.voxels_per_item});pool.push_back({entry.item,uint8_t(entry.terrain_material),{},entry.voxels_per_item});
        }
        for(uint32_t i=0;i<definition->brush_count;++i) {const auto& brush=definition->brushes[i];family.brushes.push_back({brush.item,brush.shape,brush.radius});}
        if(!Registry::valid(family)||((definition->primary.mode==KILN_TOOL_MODE_ADD||definition->secondary.mode==KILN_TOOL_MODE_ADD)&&(!definition->pool||pool.empty())))return KILN_ERR_INVALID_ARGUMENT;
        Registration registration{owner,definition->tool,{definition->tool}};
        for(uint32_t i=0;i<definition->brush_count;++i)registration.ids.push_back(definition->brushes[i].item);
        std::array<uint8_t,16> guid;std::memcpy(guid.data(),definition->identity,16);
        std::lock_guard lock(gate);
        if(registrations.size()>=4096||!next_handle)return KILN_ERR_UNAVAILABLE;
        if(!families.add(owner,std::move(family)))return KILN_ERR_INVALID_ARGUMENT;
        const auto handle=next_handle++;
        try {
            // Allocate every publication record before opening program admission. Roll back all policies if
            // either action, metadata allocation or the final atomic program-map publication fails.
            identities.emplace(definition->tool,guid);registrations.emplace(handle,registration);
        } catch(...) {remove_locked(registration);registrations.erase(handle);throw;}
        bool ok=true;
        for(auto item:registration.ids)for(uint32_t action=0;action<2;++action) {
            const auto& source=action==0?definition->primary:definition->secondary;
            KilnToolPolicyView view{};view.size=sizeof view;view.mode=source.mode;view.policy_id=source.program;view.flags=source.flags;
            std::memcpy(view.remove_cells,source.remove_cells,32);view.pool_count=uint32_t(pool.size());view.pool=pool.data();
            if(!kt::bind_tool_policy(item,action,view))ok=false;
        }
        if(ok)ok=kiln_tools_bind_programs(owner,*definition);
        if(!ok){remove_locked(registration);registrations.erase(handle);return KILN_ERR_UNAVAILABLE;}
        *out=handle;return KILN_OK;
    } catch(...) {return KILN_ERR_UNAVAILABLE;}
}
KilnResult KILN_CALL kiln_tools_remove_family(KilnOwner owner,KilnHandle handle) {
    std::lock_guard lock(gate);auto i=registrations.find(handle);if(i==registrations.end())return KILN_ERR_NOT_FOUND;
    if(i->second.owner!=owner)return KILN_ERR_INVALID_ARGUMENT;
    remove_locked(i->second);registrations.erase(i);return KILN_OK;
}
KilnResult KILN_CALL kiln_tools_material_snapshot(KilnEntity,uint32_t,KilnToolMaterialSnapshot* out) {
    if(!out||out->size<sizeof(*out))return KILN_ERR_INVALID_ARGUMENT;
    *out={};out->size=sizeof(*out);
    // Selector rows currently expose only aggregate stock. No qualified per-actor/source producer exists;
    // do not fabricate separate backpack/magic-chest counts or return another player's cached selection.
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL kiln_tools_presentation_status(KilnToolPresentationStatus* out) {
    if(!out||out->size<sizeof(*out))return KILN_ERR_INVALID_ARGUMENT;
    *out={};out->size=sizeof(*out);out->picker=kiln_tools_hook_installed(KILN_TOOL_HOOK_SELECTOR_BUILDER)!=0;
    out->preview=kiln_tools_hook_installed(KILN_TOOL_HOOK_PREVIEW)!=0;out->brushes=kiln_tools_hook_installed(KILN_TOOL_HOOK_BRUSH_PICKER)!=0;
    out->identity=kiln_tools_hook_installed(KILN_TOOL_HOOK_IDENTITY)!=0;out->reason=out->picker&&out->preview&&out->brushes&&out->identity?KILN_CAP_REASON_NONE:KILN_CAP_REASON_SYMBOL_MISSING;return KILN_OK;
}
void kiln_tools_presentation_install(const KilnApi* value,KilnOwner parent) {
    api=value;parent_owner=parent;struct Report {~Report(){report_caps();}} report;
    if(!api||child)return;
    hooks=kiln::hooks_v2(api);if(!hooks||hooks->header.size<offsetof(KilnHooksApi,code_state)+sizeof(hooks->code_state))return;
    KilnBuildInfo build{};build.size=sizeof build;
    if(api->build_info(&build)!=KILN_OK||!build.verified||!build.hooks_enabled)return;
    const bool client=build.side==KILN_SIDE_CLIENT&&std::strcmp(build.exe_sha256,"5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77")==0;
    const bool server=build.side==KILN_SIDE_SERVER&&std::strcmp(build.exe_sha256,"f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2")==0;
    if(!client&&!server)return;
    child=api->owner_create({"kiln.tools.presentation",23});
    if(!child||child==parent){child=0;return;} // Never hook or revoke under the service's parent owner.
    if(hooks->subscribe_owner_retiring(child,&owner_retiring,nullptr,&retiring)!=KILN_OK){api->owner_revoke(child);child=0;return;}
    const bool lookup_ok=install(lookup_hook,"tools/TerrainConfigLookup","4C 8B 51 38 4D 85 D2 74 ?? 45 8B 5A 04 33 C9 4D 85 DB 74 ?? 4D 63 0A 45 33 C0 66 0F 1F 44 00 00 45 85 C9 75 ?? 33 C0 EB ?? 4B 8D 04 0A 49 03 C0 38 10 74 ?? 48 FF C1 49 81 C0 B8 00 00 00",reinterpret_cast<void*>(&lookup));
    kiln_tools_report_hook(KILN_TOOL_HOOK_MATERIAL_LOOKUP,lookup_ok);
    if(client) {
        kiln_tools_report_hook(KILN_TOOL_HOOK_PREVIEW,install(preview_hook,"tools/MaterialPreview","48 89 5C 24 20 4C 89 44 24 18 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 B0 F9 FF FF 48 81 EC 50 07 00 00",reinterpret_cast<void*>(&preview)));
        kiln_tools_report_hook(KILN_TOOL_HOOK_IDENTITY,install(identity_hook,"tools/RootIdentity","48 89 5C 24 20 56 48 83 EC 20 48 8B F1 48 85 D2 0F 84 ?? ?? ?? ?? 8B 82 D8 05 00 00",reinterpret_cast<void*>(&identity)));
        const bool held_ready=held_observer.start(api,child);
        kiln_tools_report_hook(KILN_TOOL_HOOK_SELECTOR_BUILDER,held_ready&&install(selector_hook,"tools/MaterialSelector","48 89 5C 24 08 44 88 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 90 01 00 00 33 ED 4C 8D B9 38 06 00 00 48 8B DA 44 8B F5",reinterpret_cast<void*>(&selector)));
    }
    if(!client)kiln_tools_report_hook(KILN_TOOL_HOOK_SELECTOR_BUILDER,0);
    // The brush route still requires qualified caller discrimination, not merely a held root.
    kiln_tools_report_hook(KILN_TOOL_HOOK_BRUSH_PICKER,0);
}
void kiln_tools_presentation_uninstall() {
    held_provider.store(nullptr,std::memory_order_release);held_observer.stop();
    for(auto id:{KILN_TOOL_HOOK_MATERIAL_LOOKUP,KILN_TOOL_HOOK_SELECTOR_BUILDER,KILN_TOOL_HOOK_PREVIEW,KILN_TOOL_HOOK_BRUSH_PICKER,KILN_TOOL_HOOK_IDENTITY})kiln_tools_report_hook(id,0);
    {std::lock_guard lock(gate);for(const auto& [_,registration]:registrations)remove_locked(registration);registrations.clear();}
    if(child&&hooks)hooks->revoke_owner(child);if(child&&api)api->owner_revoke(child);child=0;retiring=0;
}
