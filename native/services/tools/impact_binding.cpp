// SPDX-License-Identifier: MIT
// Generic TerraformImpact binding. Existing gameplay and rules links remain in the continuation chain.
// Only native immutable values cross simulation workers. No mod delegate, tick-only service or input hook is called.
#ifdef _WIN32
#include <windows.h>
#endif
#include "impact_binding.h"
#include "impact_args.h"
#include "action_trace.h"
#include "internal.h"
#include "qualified_entry.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace {
using kiln::tools::ActionExit;
using kiln::tools::ActionDetail;
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
// The impact iterator's qword 0 is the engine's ProgramContext, not the world. Its job (+0) names the ECS storage of the
// world that runs the program at job + world/job.storage (world/JobStorage), and the server world's storage lies at
// world + world.storage (world: the address KilnApi.server_world returns): the identity kiln.items' tool wear and power
// scaling check too. docs/re/build.md section 7, corrected in 0.2.0-dev.30.8 (kiln#133).
int64_t job_storage=-1,world_storage=-1;
kiln::tools::ActionTrace action_trace;
std::atomic<uint32_t> bound_programs{};
std::atomic<uintptr_t> server_world{};
std::atomic<bool> installed{}, accepting{}, continuation_faulted{};
std::atomic<uint32_t> inflight{};
std::mutex drain_gate;
std::condition_variable drained;
KilnHandle impact_handle=0, owner_watch=0;
void KILN_CALL owner_retired(KilnOwner owner,void*) {kiln_tools_unbind_programs(owner,0);}
std::atomic<uint64_t> dispatched{}, refused{};
constexpr ActionExit admitted=ActionExit::Count; // prepare(): every check passed
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

uintptr_t refuse(KilnHookCall2* call,ActionExit exit,const ActionDetail& detail={}) {
    refused.fetch_add(1,std::memory_order_relaxed);
    action_trace.record(exit,detail);
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
ActionExit copy_exit(kt::CopyExit exit) {
    switch(exit) {
    case kt::CopyExit::Ok: return admitted;
    case kt::CopyExit::Marker: return ActionExit::Marker;
    case kt::CopyExit::NoActor: return ActionExit::NoActor;
    case kt::CopyExit::NoUsedItem: return ActionExit::NoUsedItem;
    case kt::CopyExit::Radius: return ActionExit::RadiusRange;
    case kt::CopyExit::Position: return ActionExit::Position;
    case kt::CopyExit::Aliased: return ActionExit::Aliased;
    default: return ActionExit::Layout;
    }
}
// The world check comes first: an impact another world runs is "not the server world", whatever its arguments say.
ActionExit prepare_reads(const kab::NativeCtx* ctx,const kab::ArgSlice* args,uint32_t mode,kt::ImpactCopy* copy,
                         uintptr_t* zones,uintptr_t authority) {
    if (!ctx || !args || !ctx->iter_ctx || area_offset<0 || job_storage<0 || world_storage<0) return ActionExit::NoAccess;
    const uintptr_t program_context=*reinterpret_cast<const uintptr_t*>(ctx->iter_ctx);
    *zones=*reinterpret_cast<const uintptr_t*>(ctx->iter_ctx+static_cast<uintptr_t>(area_offset));
    if (!program_context || !*zones) return ActionExit::NoAccess;
    const uintptr_t job=*reinterpret_cast<const uintptr_t*>(program_context);
    if (!job) return ActionExit::NoAccess;
    const uintptr_t storage=*reinterpret_cast<const uintptr_t*>(job+static_cast<uintptr_t>(job_storage));
    if (!authority || !storage || storage!=authority+static_cast<uintptr_t>(world_storage)) return ActionExit::NotServerWorld;
    return copy_exit(kt::copy_tool_impact_checked(*ctx,*args,mode,*copy));
}
ActionExit prepare(const kab::NativeCtx* ctx,const kab::ArgSlice* args,uint32_t mode,kt::ImpactCopy* copy,
                   uintptr_t* zones,uintptr_t authority) {
#ifdef _WIN32
    __try {
#endif
        return prepare_reads(ctx,args,mode,copy,zones,authority);
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) { return ActionExit::ReadFault; }
#endif
}
// The game's own result code of the impact it ran (0 success, 6 cancelled, 0x11 internal, 0x18 missing argument).
uint8_t result_code(uintptr_t result) {
    uint8_t code=0xff;
#ifdef _WIN32
    __try {
#endif
        if (result) code=*reinterpret_cast<const uint8_t*>(result);
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) { code=0xff; }
#endif
    return code;
}
bool continue_guarded(KilnHookCall2* call,uintptr_t* result) {
#ifdef _WIN32
    __try {
#else
    try {
#endif
        *result=hooks->call_original2(call); return true;
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    } catch (...) { return false; }
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
        bound_programs.store(static_cast<uint32_t>(next->size()),std::memory_order_relaxed);
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
        bound_programs.store(static_cast<uint32_t>(next->size()),std::memory_order_relaxed);
        bindings.store(std::move(next),std::memory_order_release);
    } catch (...) {
        // Fail closed if retirement cannot allocate its replacement map. Existing native snapshots are safe to pin,
        // but no new tool action may run until the next process start.
        installed.store(false,std::memory_order_release);
    }
}
bool kiln_tools_actions_faulted() noexcept {return continuation_faulted.load(std::memory_order_acquire);}
bool kiln_tools_actions_ready(uint32_t mode) noexcept {
    if(kiln_tools_actions_faulted() || !installed.load(std::memory_order_acquire) || !area_check || !kiln_tools_hook_installed(KILN_TOOL_HOOK_EDGE_CHECK) ||
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
    bound_programs.store(0,std::memory_order_relaxed);
    server_world.store(0,std::memory_order_release);
    // Continuations and module code remain resident. Do not null hooks while an unrelated vanilla entry can run.
}
uint64_t kiln_tools_actions_dispatched() noexcept {return dispatched.load(std::memory_order_relaxed);}
uint64_t kiln_tools_actions_refused() noexcept {return refused.load(std::memory_order_relaxed);}
kiln::tools::ActionTrace& kiln_tools_action_trace() noexcept {return action_trace;}
uint32_t kiln_tools_bound_programs() noexcept {return bound_programs.load(std::memory_order_relaxed);}

uintptr_t KILN_CALL kiln_tools_impact_detour(KilnHookCall2* call,void*) {
    // A nested unrelated impact must not inherit this tool's policy. Restore the enclosing policy afterwards.
    kt::ToolPolicyScope clear(static_cast<const KilnToolPolicyView*>(nullptr));
    kab::NativeCtx context{};
    if(!call || !hooks)return 0;
    auto map=bindings.load(std::memory_order_acquire);
    // No tool family bound (no tool mod, or every one removed): every impact is the game's own. It passes through
    // untouched, without its context even being read or counted, so a player without a tool mod keeps vanilla terraforming.
    if(!map || map->empty())return hooks->call_original2(call);
    if(!read_context(reinterpret_cast<void*>(call->args[2]),&context))return refuse(call,ActionExit::Unreadable);
    auto found=map->find(context.program_id);
    if(found==map->end()) {action_trace.other();return hooks->call_original2(call);}
    auto binding=found->second;
    ActionDetail detail;detail.program=binding->program;
    try {
        // A fault latch also closes admission: name the latch, the more specific reason, first.
        if(kiln_tools_actions_faulted())return refuse(call,ActionExit::Faulted,detail);
        Admission admission;
        if(!admission.admitted)return refuse(call,ActionExit::Closed,detail);
        if(!binding->live.load(std::memory_order_acquire))return refuse(call,ActionExit::Retired,detail);
        if(!kiln_tools_actions_ready(binding->policy.mode))return refuse(call,ActionExit::NotReady,detail);
        kt::ImpactCopy copy;
        uintptr_t zones=0;
        const auto prepared=prepare(&context,reinterpret_cast<const kab::ArgSlice*>(call->args[1]),binding->policy.mode,&copy,&zones,
            server_world.load(std::memory_order_acquire));
        if(prepared!=admitted)return refuse(call,prepared,detail);
        detail.item=copy.used_item;detail.radius=static_cast<float>(copy.radius);
        bool family_item=false,brush_ok=false;
        for(const auto& brush:binding->brushes)
            if(brush.item==copy.used_item || binding->tool==copy.used_item) {
                family_item=true;
                if(static_cast<double>(static_cast<float>(brush.radius))==copy.radius)brush_ok=true;
            }
        if(!family_item)return refuse(call,ActionExit::NotFamilyItem,detail);
        if(!brush_ok)return refuse(call,ActionExit::RadiusMismatch,detail);
        if(!area_check(zones,copy.actor,copy.hit,copy.radius))return refuse(call,ActionExit::OutsideBase,detail);
        if(!binding->live.load(std::memory_order_acquire))return refuse(call,ActionExit::Retired,detail);
        if(kiln_tools_actions_faulted())return refuse(call,ActionExit::Faulted,detail);
        kt::ToolPolicyScope scope(binding->policy);
        if(!kiln_tools_active_policy())return refuse(call,ActionExit::NoMemory,detail); // never unscoped generic terraform
        RestoreArgs restore{call,call->args[1],call->args[2]};
        call->args[1]=reinterpret_cast<uintptr_t>(&copy.arguments);
        call->args[2]=reinterpret_cast<uintptr_t>(&copy.context);
        uintptr_t result=0;
        kiln::tools::StrokeTally tally; // what the terrain kernel sees during this stroke, on this thread
        bool ran=false;
        {
            kiln::tools::StrokeScope stroke(&tally);
            // Catch native faults before returning through RAII. Never replay the continuation after a partial edit.
            ran=continue_guarded(call,&result);
        }
        if(!ran) {
            // The engine may have committed part of this stroke. Keep undo/history resident, but never admit another
            // tool action in this process, including held repeats or a later family/impact reinstallation.
            continuation_faulted.store(true,std::memory_order_release);
            accepting.store(false,std::memory_order_release);
            return refuse(call,ActionExit::ContinuationFault,detail);
        }
        dispatched.fetch_add(1,std::memory_order_relaxed);
        detail.game=result_code(result);
        action_trace.record(kiln::tools::stroke_outcome(binding->policy.mode,tally,detail.game),detail);
        return result;
    } catch (...) {return refuse(call,ActionExit::NoMemory,detail);}
}
bool kiln_tools_impact_install(const KilnApi* api,KilnOwner owner,KilnToolAreaCheck check) noexcept {
    if(!api || !owner || !check || kiln_tools_actions_faulted())return false;
    auto* h=static_cast<const KilnHooksApi*>(api->service({KILN_HOOKS_SERVICE,sizeof(KILN_HOOKS_SERVICE)-1},2));
    uintptr_t address=0;int64_t offset=-1;
    if(!h || h->header.size<offsetof(KilnHooksApi,unsubscribe_owner_retiring)+sizeof(h->unsubscribe_owner_retiring) ||
       !h->detour2 || !h->call_original2 || !h->subscribe_owner_retiring || !h->unsubscribe_owner_retiring ||
       api->symbol({"gameplay/TerraformImpact",24},&address)!=KILN_OK ||
       api->constant({"tools/area.access_zones",23},&offset)!=KILN_OK || offset<0 || offset>0x1000)return false;
    // Without the world layout an impact's world cannot be told from the authoritative one: refuse installation.
    int64_t jobs=-1,worlds=-1;
    if(api->constant({"world/job.storage",17},&jobs)!=KILN_OK || jobs<8 || jobs>0x1000 || (jobs&7) ||
       api->constant({"world.storage",13},&worlds)!=KILN_OK || worlds<=0 || worlds>0x10000000 || (worlds&7))return false;
    // Common fixed prefix on the qualified client/server entries; never inspect the registry-patched live bytes.
    constexpr uint8_t entry[]={0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x4c,0x24,0x08,
        0x55,0x53,0x56,0x41,0x55,0x41,0x56,0x41,0x57};
    if(!kt::qualified_entry(h,address,entry))return false;
    hooks=h;area_check=check;area_offset=offset;job_storage=jobs;world_storage=worlds;
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
