// SPDX-License-Identifier: MIT
#include "demand_hooks.h"
#include "services/kiln_caps.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
namespace kg {
std::recursive_mutex& hooks_publication() { static std::recursive_mutex mutex; return mutex; }
namespace {
struct Entry {
    FrameLink* link; uintptr_t target; const char* label; KilnTypedFn handler;
    std::array<KilnSigArg,6> args{}; uint32_t argc, result; KilnHandle handle=0;
};
struct Group {
    std::vector<Entry> entries;
    std::atomic<bool> installed{false}; uint32_t state=0, retries=0, next=0, reason_code=KILN_CAP_REASON_NONE; KilnOwner owner=0;
    std::string reason="resolved, not installed: no mod asked";
};
Group groups[5];
bool host_announced=false, first_world=false, legacy_fallback=false, legacy_reported=false, legacy_batch_pending=false, legacy_skip_same_pre=false;
std::filesystem::path lab_control;
bool lab_enabled=false;
constexpr uint32_t bits[]={2,4,8,16,1};
const char* names[]={"craft", "terraform", "spawn", "entities", "build"};
int index(uint32_t bit) { for(int i=0;i<5;++i) if(bit==bits[i]) return i; return -1; }
void publish(int i) {
    auto& g=groups[i]; const auto* caps=static_cast<const KilnCapsApi*>(g_api->service(kstr(KILN_CAPS_SERVICE),1));
    const std::string row=std::string("gameplay.")+names[i]+"_hooks";
    auto report=[&](const std::string& name) { if(caps && caps->report) caps->report(g_owner,kstr(name),g.state==1 ? KILN_CAP_AVAILABLE : g.state==3 ? KILN_CAP_UNAVAILABLE : KILN_CAP_PENDING,
        g.state==1 ? KILN_CAP_REASON_NONE : g.state==3 ? g.reason_code : static_cast<uint32_t>(KILN_CAP_REASON_SESSION),kstr(g.reason)); };
    report(row);
    // These pre-existing static aliases are manifest requirements in dev.28 mods. Pending would reject those mods
    // before the host can pre-arm them. Keep them available while resolved; progress belongs to the new session row.
    const char* alias=i==2 ? "gameplay.spawn_replace" : i==3 ? "gameplay.entity_events" : nullptr;
    if(alias && caps && caps->report) caps->report(g_owner,kstr(alias),g.state==3 ? KILN_CAP_UNAVAILABLE : KILN_CAP_AVAILABLE,
        g.state==3 ? g.reason_code : static_cast<uint32_t>(KILN_CAP_REASON_NONE),kstr(g.state==3 ? g.reason : "resolved; see the hook group row for installation state"));
}
const KilnHooksApi* installer() {
    const auto* h=kiln::hooks_v2(g_api);
    return h && h->header.size>=offsetof(KilnHooksApi,detour_typed_checked)+sizeof(h->detour_typed_checked) &&
           h->typed_install_ready && h->detour_typed_checked && h->call_original_frame ? h : nullptr;
}
void refuse(int i, const std::string& why) {
    auto& g=groups[i]; g.state=3; g.reason_code=KILN_CAP_REASON_FAULTED; g.reason="refused: "+why+"; installed pass-through targets "+std::to_string(g.next);
    KG_W("kiln.gameplay: %s hooks %s",names[i],g.reason.c_str()); publish(i);
}
unsigned pre_writes=0;  // late installer calls in the current hooks_poll (one Pre)
void advance(int i, const KilnHooksApi* h, unsigned& budget) {
    auto& g=groups[i];
    // While a world runs, each checked write stops the game's other threads (kiln.hooks logs that time at debug and
    // warns above 50 ms): the whole call is timed here and logged at info, so a late install's cost is in kiln.log.
    const bool late=h->typed_install_ready()==1;
    while(g.next<g.entries.size() && budget) {
        auto& e=g.entries[g.next];
        if(!e.target) { refuse(i,std::string(e.label)+" did not resolve"); return; }
        KilnSig sig{}; sig.size=sizeof sig; sig.argc=e.argc; sig.args=e.args.data(); sig.ret_kind=e.result;
        // Services used to precede plugin-owned priority-zero links. Pin the supported peers explicitly so a
        // late join keeps that order. EMF's priority-one newest-first links still precede gameplay; rules has its
        // own before-kiln.gameplay constraint. Creature lifetime registration precedes gameplay in service load order.
        const auto order=kiln::hook_detail::make_order(0,e.label,"kiln.set,kiln.shroudforge,kiln.npcai","kiln.creatures,kiln.build,kiln.rules");
        FrameLink::api=h; --budget;
        // Lab negative control only: a second marker was required at service load. Refuse BEFORE the installer,
        // so the lab cannot accidentally patch the selected target or anything after it.
        std::string forced; unsigned target=0;
        if(lab_enabled) { std::ifstream control(lab_control / "gameplay.unchecked"); control >> forced >> target; }
        const bool inject=forced==names[i] && target==g.next;
        const auto t0=std::chrono::steady_clock::now();
        const auto rc=inject ? static_cast<KilnResult>(KILN_HOOKS_ERR_UNCHECKED) :
            h->detour_typed_checked(g_owner,e.target,&order,&sig,e.handler,e.link,&e.handle);
        if(inject) KG_W("kiln.gameplay: LAB %s target %u unchecked attempt %u (nothing written)",names[i],g.next,g.retries+1);
        else if(late) {
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
            KG_I("kiln.gameplay: %s %s during play: the installer call took %.1f ms on the world's thread (write %u in this Pre)",
                 e.label,rc==KILN_OK ? "went in" : "was refused",ms,++pre_writes);
        }
        if(rc!=KILN_OK) {
            char text[512]{}; size_t n=0;
            if(!inject && h->typed_refusal) h->typed_refusal(text,sizeof text,&n);
            if(rc==KILN_HOOKS_ERR_UNCHECKED && ++g.retries<3) {
                g.reason=std::string("waiting: ")+e.label+" thread check refused; retry at next safe Pre"; publish(i); return;
            }
            refuse(i,std::string(e.label)+" (error "+std::to_string(rc)+"): "+
                (rc==KILN_HOOKS_ERR_UNCHECKED ? "thread check refused on three attempts; see kiln.log" : text)); return;
        }
        ++g.next; g.retries=0;
    }
    if(g.next==g.entries.size()) {
        g.state=1; g.reason="installed"; g.installed.store(true,std::memory_order_release); publish(i);
        KG_I("kiln.gameplay: %s hooks installed on first use by %llu",names[i],static_cast<unsigned long long>(g.owner));
    }
}
}
void hook_register(uint32_t bit,FrameLink* link,uintptr_t target,const char* label,KilnTypedFn handler,
                   std::initializer_list<uint32_t> args,uint32_t result) {
    int i=index(bit); auto& g=groups[i]; std::lock_guard lock(hooks_publication());
    Entry e{}; e.link=link; e.target=target; e.label=label; e.handler=handler; e.argc=static_cast<uint32_t>(args.size()); e.result=result;
    unsigned j=0; for(auto a:args) e.args[j++].kind=a;
    g.entries.push_back(e);
    if(!target) { g.state=3; g.reason_code=KILN_CAP_REASON_SYMBOL_MISSING; g.reason=std::string("refused: ")+label+" symbols/layout did not resolve"; }
    publish(i);
}
bool hooks_installed(uint32_t bit) { int i=index(bit); return i>=0 && groups[i].installed.load(std::memory_order_acquire); }
bool hooks_request(uint32_t mask,KilnOwner owner) {
    if(mask & ~KILN_GAMEPLAY_HOOK_ALL) return false;
    if(mask==0) { std::lock_guard lock(hooks_publication()); host_announced=true; return true; }
    bool ok=true;
    for(int i=0;i<5;++i) if(mask & bits[i]) {
        auto& g=groups[i]; std::lock_guard lock(hooks_publication());
        if(g.state==0) {
            g.owner=owner; g.state=2; g.reason="waiting for safe Pre; events and rules begin after installation (earlier events are not replayed)"; publish(i);
            const auto* h=installer();
            if(!h) refuse(i,"kiln.hooks has no checked typed installer");
            else if(h->typed_install_ready()==2) { unsigned budget=6; advance(i,h,budget); }
            else KG_I("kiln.gameplay: %s hooks waiting for safe Pre (owner %llu); events and rules begin after installation",names[i],static_cast<unsigned long long>(owner));
        }
        ok=(g.state!=3) && ok;
    }
    return ok;
}
void legacy_result() {
    if(!legacy_fallback || legacy_reported) return;
    bool complete=true, refused=false;
    for(auto& g:groups) { complete &= g.state==1 || g.state==3; refused |= g.state==3; }
    if(complete) { legacy_reported=true;
        if(!refused) KG_I("no demand-aware host: all gameplay hooks installed as before");
        else KG_W("no demand-aware host: requested all gameplay hooks; some refused (see group rows)"); }
}
void hooks_poll() {
    const auto* h=installer(); if(!h || !h->typed_install_ready()) return;
    std::lock_guard lock(hooks_publication());
    pre_writes=0;
    if(legacy_skip_same_pre) { legacy_skip_same_pre=false; return; }
    // Legacy-only first-world batch precedes old Host's first Pre/OnLoad. A checked refusal still retries later;
    // all ordinary late demand (and those retries) shares the two-target budget.
    unsigned budget=legacy_batch_pending ? 10 : 2;
    legacy_batch_pending=false;
    for(int i=0;i<5 && budget;++i) if(groups[i].state==2) advance(i,h,budget);
    legacy_result();
}
void hooks_init_control() {
    char path[4096]{}; size_t n=0;
    if(!g_api->path || g_api->path(KILN_PATH_CONTROL,path,sizeof path,&n)!=KILN_OK || !n || n>=sizeof path) return;
    lab_control=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path),n));
    std::error_code ec;
    lab_enabled=std::filesystem::exists(lab_control / "gameplay.lab",ec);
    if(lab_enabled) KG_W("kiln.gameplay: LAB unchecked control enabled by gameplay.lab; never enable on a player's server");
    else if(std::filesystem::exists(lab_control / "gameplay.unchecked",ec))
        KG_W("kiln.gameplay: gameplay.unchecked IGNORED without gameplay.lab at service load");
}
void hooks_world_started(bool world_notification) {
    std::lock_guard lock(hooks_publication());
    if(first_world) return;
    first_world=true;
    if(!host_announced) {
        legacy_fallback=true;
        const auto* h=installer();
        const uint32_t ready=h ? h->typed_install_ready() : 0;
        hooks_request(KILN_GAMEPLAY_HOOK_ALL,g_owner);
        // No-world requests already attempted each group once. A refusal must not get an immediate second try.
        if(ready!=2) {
            legacy_batch_pending=true;
            if(h && h->typed_install_ready()) {
                hooks_poll();
                // begin_world is inside the first Pre. Its ordinary gameplay subscriber still follows; retries
                // belong to a LATER Pre, not that second callback in the same one.
                legacy_skip_same_pre=world_notification;
            }
        }
        legacy_result();
    }
}
bool build_request(KilnOwner owner) { return hooks_request(KILN_GAMEPLAY_HOOK_BUILD,owner); }
bool build_installed() { return hooks_installed(KILN_GAMEPLAY_HOOK_BUILD); }
uint32_t build_state() { std::lock_guard lock(hooks_publication()); return groups[4].state; }
uint32_t build_mask() { std::lock_guard lock(hooks_publication()); return (1u<<groups[4].next)-1; }
std::string build_reason() { std::lock_guard lock(hooks_publication()); return groups[4].reason; }
bool hooks_interest(uint32_t mask,KilnOwner owner) {
    uint32_t groups=0;
    if(mask & (1u<<KILN_GAMEPLAY_CRAFTED)) groups|=KILN_GAMEPLAY_HOOK_CRAFT;
    if(mask & (1u<<KILN_GAMEPLAY_BUILD)) groups|=KILN_GAMEPLAY_HOOK_BUILD|KILN_GAMEPLAY_HOOK_TERRAFORM;
    if(mask & (1u<<KILN_GAMEPLAY_SPAWN)) groups|=KILN_GAMEPLAY_HOOK_SPAWN;
    if(mask & ((1u<<KILN_GAMEPLAY_ENTITY_JOINED)|(1u<<KILN_GAMEPLAY_ENTITY_LEFT))) groups|=KILN_GAMEPLAY_HOOK_ENTITIES;
    return !groups || hooks_request(groups,owner);
}
bool hooks_denies(uint32_t deny,KilnOwner owner) {
    uint32_t mask=0;
    if(deny & KILN_GAMEPLAY_DENY_TERRAFORM) mask|=KILN_GAMEPLAY_HOOK_TERRAFORM;
    if(deny & KILN_GAMEPLAY_DENY_SPAWN) mask|=KILN_GAMEPLAY_HOOK_SPAWN;
    if(mask && !hooks_request(mask,owner)) return false;
    return (!(mask & KILN_GAMEPLAY_HOOK_TERRAFORM)||hooks_installed(KILN_GAMEPLAY_HOOK_TERRAFORM)) &&
           (!(mask & KILN_GAMEPLAY_HOOK_SPAWN)||hooks_installed(KILN_GAMEPLAY_HOOK_SPAWN));
}
void hooks_status(KilnGameplayStatus* s) {
    for(int i=0;i<4;++i) { auto& g=groups[i]; std::lock_guard lock(hooks_publication()); auto& out=s->hook_groups[i];
        out.state=g.state; out.installed_targets=g.next; std::snprintf(out.reason,sizeof out.reason,"%s",g.reason.c_str()); }
}
}
