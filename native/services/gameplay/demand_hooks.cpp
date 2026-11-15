// SPDX-License-Identifier: MIT
#include "demand_hooks.h"
#include "services/kiln_caps.h"
#include <algorithm>
#include <cstdio>
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
Group groups[4];
const char* names[]={"craft", "terraform", "spawn", "entities"};
int index(uint32_t bit) { for(int i=0;i<4;++i) if(bit==(2u<<i)) return i; return -1; }
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
void advance(int i, const KilnHooksApi* h, unsigned& budget) {
    auto& g=groups[i];
    while(g.next<g.entries.size() && budget) {
        auto& e=g.entries[g.next];
        if(!e.target) { refuse(i,std::string(e.label)+" did not resolve"); return; }
        KilnSig sig{}; sig.size=sizeof sig; sig.argc=e.argc; sig.args=e.args.data(); sig.ret_kind=e.result;
        // Services used to precede plugin-owned priority-zero links. Pin the supported peers explicitly so a
        // late join keeps that order. EMF's priority-one newest-first links still precede gameplay; rules has its
        // own before-kiln.gameplay constraint. Creature lifetime registration precedes gameplay in service load order.
        const auto order=kiln::hook_detail::make_order(0,e.label,"kiln.set,kiln.shroudforge,kiln.npcai","kiln.creatures");
        FrameLink::api=h; --budget;
        const auto rc=h->detour_typed_checked(g_owner,e.target,&order,&sig,e.handler,e.link,&e.handle);
        if(rc!=KILN_OK) {
            char text[512]{}; size_t n=0;
            if(h->typed_refusal) h->typed_refusal(text,sizeof text,&n);
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
    if(!mask || (mask & ~KILN_GAMEPLAY_HOOK_ALL)) return false;
    bool ok=true;
    if(mask & KILN_GAMEPLAY_HOOK_BUILD) ok=build_request(owner);
    for(int i=0;i<4;++i) if(mask & (2u<<i)) {
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
void hooks_poll() {
    const auto* h=installer(); if(!h || !h->typed_install_ready()) return;
    unsigned budget=2;
    for(int i=0;i<4 && budget;++i) { auto& g=groups[i]; std::lock_guard lock(hooks_publication()); if(g.state==2) advance(i,h,budget); }
}
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
