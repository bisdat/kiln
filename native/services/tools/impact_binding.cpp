// SPDX-License-Identifier: MIT
// Generic TerraformImpact binding. Existing gameplay and rules links remain in the continuation chain.
// Only native immutable values cross simulation workers. No mod delegate, tick-only service or input hook is called.
#ifdef _WIN32
#include <windows.h>
#endif
#include "impact_binding.h"
#include "impact_args.h"
#include "internal.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace {
struct Binding {
    mutable std::atomic<bool> live{true};
    KilnOwner owner{};
    uint32_t tool{}, action{}, program{};
    KilnToolPolicyView policy{};
    std::vector<KilnToolPoolEntry> pool;
    std::vector<KilnToolBrush> brushes;
};
using Bindings=std::unordered_map<uint32_t,std::shared_ptr<const Binding>>;
std::atomic<std::shared_ptr<const Bindings>> bindings;
std::mutex binding_gate;
const KilnHooksApi* hooks=nullptr;
KilnToolAreaCheck area_check=nullptr;
int64_t area_offset=-1;
std::atomic<uintptr_t> server_world{};
std::atomic<bool> installed{}, accepting{};
std::atomic<uint32_t> inflight{};
std::mutex drain_gate;
std::condition_variable drained;
KilnHandle impact_handle=0, owner_watch=0;
void KILN_CALL owner_retired(KilnOwner owner,void*) {kiln_tools_unbind_programs(owner,0);}
std::atomic<uint64_t> dispatched{}, refused{};
struct Admission {
    bool admitted=false;
    Admission() {
        std::lock_guard lock(drain_gate);
        if(!accepting.load(std::memory_order_acquire))return;
        inflight.fetch_add(1,std::memory_order_acq_rel);
        admitted=true;
    }
    ~Admission(){if(admitted){inflight.fetch_sub(1,std::memory_order_acq_rel);drained.notify_all();}}
};

uintptr_t refuse(KilnHookCall2* call) {
    refused.fetch_add(1,std::memory_order_relaxed);
    auto* out=reinterpret_cast<uint8_t*>(call->args[0]);
    if (out) { out[0]=0x18; std::memset(out+4,0,4); out[8]=0; }
    return reinterpret_cast<uintptr_t>(out);
}
bool read_context(const void* source,kab::NativeCtx* out) {
#ifdef _WIN32
    __try {
#endif
        if (!source) return false;
        std::memcpy(out,source,sizeof(*out)); return true;
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}
bool prepare(const kab::NativeCtx* ctx,const kab::ArgSlice* args,uint32_t mode,kt::ImpactCopy* copy,
             uintptr_t* zones,uintptr_t* world) {
#ifdef _WIN32
    __try {
#endif
        if (!ctx || !args || !ctx->iter_ctx || area_offset<0) return false;
        *world=*reinterpret_cast<const uintptr_t*>(ctx->iter_ctx);
        *zones=*reinterpret_cast<const uintptr_t*>(ctx->iter_ctx+static_cast<uintptr_t>(area_offset));
        return *world && *zones && kt::copy_tool_impact(*ctx,*args,mode,*copy);
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}
bool continue_guarded(KilnHookCall2* call,uintptr_t* result) {
#ifdef _WIN32
    __try {
#endif
        *result=hooks->call_original2(call); return true;
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}
struct RestoreArgs {
    KilnHookCall2* call; uintptr_t args,context;
    ~RestoreArgs() {call->args[1]=args;call->args[2]=context;}
};
}

bool kiln_tools_bind_programs(KilnOwner owner,const KilnToolFamilyDef& family) noexcept {
    if (!owner || !family.tool || !family.brushes || !family.brush_count || family.brush_count>64 ||
        family.material_count>64 || (family.material_count && !family.materials)) return false;
    try {
        std::lock_guard lock(binding_gate);
        auto current=bindings.load(std::memory_order_acquire);
        auto next=current?std::make_shared<Bindings>(*current):std::make_shared<Bindings>();
        const KilnToolFamilyAction* actions[]={&family.primary,&family.secondary};
        for (uint32_t action=0;action<2;++action) {
            const auto& source=*actions[action];
            if (source.mode==KILN_TOOL_MODE_NONE) continue;
            if (!source.program || next->contains(source.program) || source.mode>KILN_TOOL_MODE_ADD ||
                source.flags!=KILN_TOOL_TERRAIN_ONLY) return false;
            auto b=std::make_shared<Binding>();
            b->owner=owner;b->tool=family.tool;b->action=action;b->program=source.program;
            b->policy.size=sizeof(b->policy);b->policy.mode=source.mode;b->policy.policy_id=source.program;
            b->policy.flags=source.flags;
            std::memcpy(b->policy.remove_cells,source.remove_cells,32);
            for (uint32_t i=0;i<family.material_count;++i) {
                const auto& m=family.materials[i];
                if (!m.item || !m.terrain_material || m.terrain_material>=128 || !m.voxels_per_item) return false;
                b->pool.push_back({m.item,static_cast<uint8_t>(m.terrain_material),{},m.voxels_per_item});
            }
            b->policy.pool_count=static_cast<uint32_t>(b->pool.size());b->policy.pool=b->pool.data();
            b->brushes.assign(family.brushes,family.brushes+family.brush_count);
            for (const auto& brush:b->brushes)
                if (!brush.item || brush.shape!=KILN_TOOL_SHAPE_SPHERE || !std::isfinite(brush.radius) ||
                    brush.radius<0.25 || brush.radius>8) return false;
            next->emplace(source.program,std::move(b));
        }
        bindings.store(std::move(next),std::memory_order_release);
        return true;
    } catch (...) {return false;}
}
void kiln_tools_unbind_programs(KilnOwner owner,uint32_t tool) noexcept {
    try {
        std::lock_guard lock(binding_gate);
        auto current=bindings.load(std::memory_order_acquire);if(!current)return;
        auto next=std::make_shared<Bindings>(*current);
        for(auto i=next->begin();i!=next->end();) {
            if(i->second->owner==owner && (!tool || i->second->tool==tool)) {
                i->second->live.store(false,std::memory_order_release);
                i=next->erase(i);
            } else ++i;
        }
        bindings.store(std::move(next),std::memory_order_release);
    } catch (...) {
        // Fail closed if retirement cannot allocate its replacement map. Existing native snapshots are safe to pin,
        // but no new tool action may run until the next process start.
        installed.store(false,std::memory_order_release);
    }
}
bool kiln_tools_actions_ready(uint32_t mode) noexcept {
    if(!installed.load(std::memory_order_acquire) || !area_check || !kiln_tools_hook_installed(KILN_TOOL_HOOK_EDGE_CHECK) ||
       !kiln_tools_hook_installed(KILN_TOOL_HOOK_HISTORY_COST))return false;
    if(mode==KILN_TOOL_POLICY_REMOVE)return kiln_tools_hook_installed(KILN_TOOL_HOOK_KERNEL_FILTER) &&
        kiln_tools_hook_installed(KILN_TOOL_HOOK_BUILDING_SUPPRESS);
    if(mode==KILN_TOOL_POLICY_ADD)return kiln_tools_hook_installed(KILN_TOOL_HOOK_COST) &&
        kiln_tools_hook_installed(KILN_TOOL_HOOK_MATERIAL_LOOKUP);
    return false;
}
void kiln_tools_impact_world(uintptr_t world) noexcept {server_world.store(world,std::memory_order_release);}
bool kiln_tools_impact_close() noexcept {
    std::unique_lock lock(drain_gate);
    accepting.store(false,std::memory_order_release);
    installed.store(false,std::memory_order_release);
    return drained.wait_for(lock,std::chrono::milliseconds(250),[]{return inflight.load(std::memory_order_acquire)==0;});
}
void kiln_tools_impact_uninstall() noexcept {
    if(!kiln_tools_impact_close())return; // process-resident: retain all pointers if an admitted call outlasted the bound
    if(hooks && impact_handle && hooks->revoke)hooks->revoke(impact_handle);
    if(hooks && owner_watch && hooks->unsubscribe_owner_retiring)hooks->unsubscribe_owner_retiring(owner_watch);
    impact_handle=owner_watch=0;
    bindings.store({},std::memory_order_release);
    server_world.store(0,std::memory_order_release);
    // Continuations and module code remain resident. Do not null hooks while an unrelated vanilla entry can run.
}
uint64_t kiln_tools_actions_dispatched() noexcept {return dispatched.load(std::memory_order_relaxed);}
uint64_t kiln_tools_actions_refused() noexcept {return refused.load(std::memory_order_relaxed);}

uintptr_t KILN_CALL kiln_tools_impact_detour(KilnHookCall2* call,void*) {
    // A nested unrelated impact must not inherit this tool's policy. Restore the enclosing policy afterwards.
    kt::ToolPolicyScope clear(static_cast<const KilnToolPolicyView*>(nullptr));
    kab::NativeCtx context{};
    if(!call || !hooks)return 0;
    if(!read_context(reinterpret_cast<void*>(call->args[2]),&context))return refuse(call);
    auto map=bindings.load(std::memory_order_acquire);
    auto found=map?map->find(context.program_id):Bindings::const_iterator{};
    if(!map || found==map->end())return hooks->call_original2(call);
    auto binding=found->second;
    try {
        Admission admission;
        if(!admission.admitted || !binding->live.load(std::memory_order_acquire) ||
           !kiln_tools_actions_ready(binding->policy.mode))return refuse(call);
        kt::ImpactCopy copy;
        uintptr_t zones=0,world=0;
        if(!prepare(&context,reinterpret_cast<const kab::ArgSlice*>(call->args[1]),binding->policy.mode,&copy,&zones,&world) ||
           world!=server_world.load(std::memory_order_acquire))return refuse(call);
        bool brush_ok=false;
        for(const auto& brush:binding->brushes)
            if((brush.item==copy.used_item || binding->tool==copy.used_item) &&
               static_cast<double>(static_cast<float>(brush.radius))==copy.radius)brush_ok=true;
        if(!brush_ok || !area_check(zones,copy.actor,copy.hit,copy.radius) ||
           !binding->live.load(std::memory_order_acquire))return refuse(call);
        kt::ToolPolicyScope scope(binding->policy);
        if(!kiln_tools_active_policy())return refuse(call); // allocation failed: never unscoped generic terraform
        RestoreArgs restore{call,call->args[1],call->args[2]};
        call->args[1]=reinterpret_cast<uintptr_t>(&copy.arguments);
        call->args[2]=reinterpret_cast<uintptr_t>(&copy.context);
        uintptr_t result=0;
        // Catch native faults before returning through RAII. Never replay the continuation after a partial edit.
        if(!continue_guarded(call,&result))return refuse(call);
        dispatched.fetch_add(1,std::memory_order_relaxed);
        return result;
    } catch (...) {return refuse(call);}
}
bool kiln_tools_impact_install(const KilnApi* api,KilnOwner owner,KilnToolAreaCheck check) noexcept {
    if(!api || !owner || !check)return false;
    auto* h=static_cast<const KilnHooksApi*>(api->service({KILN_HOOKS_SERVICE,sizeof(KILN_HOOKS_SERVICE)-1},2));
    uintptr_t address=0;int64_t offset=-1;
    if(!h || h->header.size<offsetof(KilnHooksApi,unsubscribe_owner_retiring)+sizeof(h->unsubscribe_owner_retiring) ||
       !h->detour2 || !h->call_original2 || !h->subscribe_owner_retiring || !h->unsubscribe_owner_retiring ||
       api->symbol({"gameplay/TerraformImpact",24},&address)!=KILN_OK ||
       api->constant({"tools/area.access_zones",23},&offset)!=KILN_OK || offset<0 || offset>0x1000)return false;
    hooks=h;area_check=check;area_offset=offset;
    if(h->subscribe_owner_retiring(owner,&owner_retired,nullptr,&owner_watch)!=KILN_OK)return false;
    // Run outside gameplay/rules: both see the copied generic arguments and retain their normal protections.
    KilnHookOrder order{sizeof(order),100,{"tools/TerrainAction",19},{"kiln.gameplay,kiln.rules",24},{},0};
    KilnHandle handle=0;
    const bool on=h->detour2(owner,address,&order,3,&kiln_tools_impact_detour,nullptr,&handle)==KILN_OK;
    if(!on) {h->unsubscribe_owner_retiring(owner_watch);owner_watch=0;}
    impact_handle=handle;
    accepting.store(on,std::memory_order_release);
    installed.store(on,std::memory_order_release);
    return on;
}
