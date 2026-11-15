// SPDX-License-Identifier: MIT
// Exercise actual native subscriber/tick API consumers, hooks and zones with fake host tables.
#include "../gameplay.cpp"
#include "../demand_hooks.h"
#include <cstdio>
#include <thread>
#include <map>
#include "services/kiln_caps.h"
namespace kg {
KilnResult component_change(KilnOwner,KilnEntity,uint32_t,const void*,uint32_t,uint32_t) { return KILN_ERR_UNAVAILABLE; }
KilnResult workshop_recipes(uint32_t,uint32_t*,uint32_t,uint32_t*) { return KILN_ERR_UNAVAILABLE; }
KilnResult recipe(uint32_t,KilnGameplayRecipe*) { return KILN_ERR_UNAVAILABLE; }
KilnResult watch(KilnEntity,bool) { return KILN_ERR_UNAVAILABLE; }
KilnResult inject(const KilnGameplayInject*) { return KILN_ERR_UNAVAILABLE; }
bool program_active() { return false; }
void discard_status(uint64_t* a,uint64_t* b) { *a=*b=0; }

bool seh_call(WalkFn,void*,unsigned long*) { return false; }
bool seh_build_target(uintptr_t) { return true; }
bool seh_subscriber(KilnGameplayFn f,KilnGameplayEvent* e,void* u,uint32_t* v,unsigned long*) { *v=f(e,u); return true; }
bool seh_get_transform(uintptr_t,KilnEntity,const int64_t**) { return false; }
bool seh_get_component(uintptr_t,uint32_t,KilnEntity,void**,uint32_t*) { return false; }
}
static unsigned ready=2, installs=0, originals=0, checks=0, failed=0, retry=0;
static uint32_t interest=0; static bool partial=false;
static std::map<std::string,uint32_t> rows; static KilnCapsApi caps{};
static KilnHooksApi ht{}; static KilnApi api{};
struct Link { KilnTypedFn fn; void* user; };
static std::vector<Link> links;
#define CHECK(x) do { ++checks; if(!(x)) { ++failed; std::printf("FAIL %d %s\n",__LINE__,#x); } } while(0)
static uint32_t KILN_CALL event(KilnGameplayEvent*,void*) { return interest; }
int main(int argc,char** argv) {
    const std::string mode=argc>1 ? argv[1] : "craft";
    ht.header={sizeof ht,2};
    ht.typed_install_ready=[]()->uint32_t { return ready; };
    ht.raw_detour=[](KilnOwner,uintptr_t,const KilnHookOrder*,void*,void**,KilnHandle*)->KilnResult { ++installs; return KILN_ERR_INTERNAL; };
    ht.detour_typed_checked=[](KilnOwner,uintptr_t,const KilnHookOrder* order,const KilnSig* sig,KilnTypedFn f,void* u,KilnHandle* out)->KilnResult {
        ++installs; CHECK(order->priority==0 && kg::sv(order->before)=="kiln.set,kiln.shroudforge,kiln.npcai" && kg::sv(order->after)=="kiln.creatures");
        CHECK(sig->argc>=2 && sig->argc<=6); if(retry && (!partial || installs>1)) { --retry; return KILN_HOOKS_ERR_UNCHECKED; }
        *out=links.size()+1; links.push_back({f,u}); return KILN_OK;
    };
    ht.call_original_frame=[](KilnCallFrame* f)->KilnResult { ++originals; f->rax=7; return KILN_OK; };
    ht.caller=[](KilnHandle)->uintptr_t { return 0; };
    caps.report=[](KilnOwner,KilnStr name,uint32_t state,uint32_t,KilnStr)->KilnResult { rows[std::string(kg::sv(name))]=state; return KILN_OK; };
    api.size=sizeof api; api.service=[](KilnStr name,uint32_t)->const void* { return kg::sv(name)=="kiln.hooks" ? static_cast<const void*>(&ht) : kg::sv(name)=="kiln.caps" ? &caps : nullptr; };
    api.session=[](KilnSessionInfo* s)->KilnResult { s->worlds[0]=ready==2 ? 0 : 1; return KILN_OK; };
    api.server_world=[]()->uintptr_t { return 0; }; api.on_tick_thread=[]()->bool { return true; };
    api.log=[](KilnOwner,int,KilnStr){}; api.feature=[](KilnStr)->bool { return true; };
    api.symbol=[](KilnStr,uintptr_t* p)->KilnResult { *p=100; return KILN_OK; };
    api.constant=[](KilnStr name,int64_t* p)->KilnResult {
        auto s=kg::sv(name); *p=s=="spawn/storage.entity_map_cap" ? 0x110 : s=="spawn/storage.entity_records" ? 0x140 :
             s=="spawn/storage.entity_map" ? 0x100 : 0x10; return KILN_OK;
    };
    kg::g_api=&api; kg::g_owner=1; kg::L.world_storage=8;
    kg::F.craft=kg::F.terraform=kg::F.spawn=true; kg::fn.craft=kg::fn.terraform=kg::fn.queue_spawn=100;
    kg::hooks_install(); kg::entities_install(); CHECK(installs==0);
    CHECK(rows["gameplay.craft_hooks"]==KILN_CAP_PENDING); CHECK(rows["gameplay.terraform_hooks"]==KILN_CAP_PENDING);
    CHECK(rows["gameplay.spawn_hooks"]==KILN_CAP_PENDING); CHECK(rows["gameplay.entities_hooks"]==KILN_CAP_PENDING);
    CHECK(rows["gameplay.spawn_replace"]==KILN_CAP_AVAILABLE); CHECK(rows["gameplay.entity_events"]==KILN_CAP_AVAILABLE);
    if(mode=="idle") {
        uint8_t from[16]={1},to[16]={2}; CHECK(kg::hooks_spawn_replace(77,from,to,0)==KILN_OK);
        CHECK(kg::hooks_spawn_replace_clear(77)==0); CHECK(kg::entity_filter(0)==KILN_OK); kg::hooks_poll(); CHECK(installs==0);
        std::printf("remaining idle: %u checks, %u failures\n",checks,failed); return failed ? 1 : 0;
    }
    unsigned group=mode.find("terraform")!=std::string::npos ? 4 : mode.find("spawn")!=std::string::npos ? 8 : mode.find("entities")!=std::string::npos ? 16 : 2;
    const bool late=mode.find("late")!=std::string::npos, denied=mode.find("refuse")!=std::string::npos;
    if(late || denied) ready=0;
    if(denied) retry=3;
    partial=mode.find("partial")!=std::string::npos;
    if(mode.find("threads")!=std::string::npos) {
        std::thread a([&] { kg::t_request_hooks(77,group); }), b([&] { kg::t_request_hooks(88,group); }); a.join(); b.join();
    } else if(mode.find("update")!=std::string::npos) {
        KilnGameplayZone zone{}; zone.size=sizeof zone; zone.shape=KILN_GAMEPLAY_ZONE_SPHERE; zone.radius=10;
        uint64_t empty=0; zone.allowed_steam_ids=&empty; zone.deny=KILN_GAMEPLAY_DENY_INTERACT; uint32_t id=0;
        CHECK(kg::zone_add(77,&zone,&id)==KILN_OK);
        zone.deny=group==4 ? KILN_GAMEPLAY_DENY_TERRAFORM : KILN_GAMEPLAY_DENY_SPAWN;
        CHECK(kg::zone_update(id,&zone)==(late ? KILN_ERR_UNAVAILABLE : KILN_OK));
        CHECK(kg::zones_any(zone.deny)==!late);
    } else if(mode.find("zone")!=std::string::npos) {
        KilnGameplayZone zone{}; zone.size=sizeof zone; zone.shape=KILN_GAMEPLAY_ZONE_SPHERE; zone.radius=10;
        zone.deny=group==4 ? KILN_GAMEPLAY_DENY_TERRAFORM : KILN_GAMEPLAY_DENY_SPAWN;
        uint64_t empty=0; zone.allowed_steam_ids=&empty; uint32_t id=0;
        CHECK(kg::zone_add(77,&zone,&id)==(late ? KILN_ERR_UNAVAILABLE : KILN_OK));
        CHECK(kg::zone_count()==(late ? 0 : 1));
    } else if(mode.find("replace")!=std::string::npos) {
        uint8_t guid[16]={1},to[16]={2}; CHECK(kg::hooks_spawn_replace(77,guid,to,1)==KILN_OK);
    } else if(mode.find("filter")!=std::string::npos) CHECK(kg::entity_filter(KILN_GAMEPLAY_ENTITY_ALL)==KILN_OK);
    else if(mode.find("tick")!=std::string::npos) {
        KilnHandle h=0; CHECK(kg::t_subscribe(77,1,&event,nullptr,&h)==KILN_OK);
        interest=1u<<(group==2 ? KILN_GAMEPLAY_CRAFTED : group==4 ? KILN_GAMEPLAY_BUILD : group==8 ? KILN_GAMEPLAY_SPAWN : KILN_GAMEPLAY_ENTITY_JOINED);
        kg::tick_subscribers();
    } else {
        KilnHandle h=0;
        const auto kind=group==2 ? KILN_GAMEPLAY_CRAFTED : group==8 ? KILN_GAMEPLAY_SPAWN : KILN_GAMEPLAY_ENTITY_LEFT;
        if(group==4 && mode.find("subscribe")!=std::string::npos)
            CHECK(kg::t_subscribe(77,1u<<KILN_GAMEPLAY_BUILD,&event,nullptr,&h)==KILN_ERR_UNAVAILABLE); // missing five-piece symbols; terraform still requested
        else if(group==4) CHECK(kg::t_request_hooks(77,group)==KILN_OK); // Build itself retains its separate stop policy.
        else CHECK(kg::t_subscribe(77,1u<<kind,&event,nullptr,&h)==KILN_OK);
        if(h) CHECK(kg::t_unsubscribe(h)==KILN_OK);
    }
    if(late || denied) {
        CHECK(installs==0); KilnGameplayStatus s{}; kg::hooks_status(&s); CHECK(s.hook_groups[group==2?0:group==4?1:group==8?2:3].state==2);
        ready=1; kg::hooks_poll(); if(denied) { kg::hooks_poll(); kg::hooks_poll(); }
    }
    const auto count=group==16 ? 2u : 1u;
    CHECK(installs==(denied ? (partial ? 4u : 3u) : count)); CHECK(kg::hooks_installed(group)==!denied);
    KilnGameplayStatus status{}; kg::hooks_status(&status); const auto& s=status.hook_groups[group==2?0:group==4?1:group==8?2:3];
    CHECK(s.state==(denied ? 3u : 1u)); CHECK(s.reason[0]);
    const char* names[]={"craft","terraform","spawn","entities"};
    CHECK(rows[std::string("gameplay.")+names[group==2?0:group==4?1:group==8?2:3]+"_hooks"]==(denied ? KILN_CAP_UNAVAILABLE : KILN_CAP_AVAILABLE));
    if(mode.find("replace")!=std::string::npos) CHECK(kg::hooks_spawn_replace_clear(77)==1u);
    if(mode.find("filter")!=std::string::npos) CHECK(kg::entity_filter(0)==KILN_OK);
    CHECK(kg::hooks_request(group,99)==!denied); kg::hooks_poll(); CHECK(installs==(denied ? (partial ? 4u : 3u) : count));
    if(partial) { CHECK(links.size()==1); KilnCallFrame f{}; CHECK(links[0].fn(&f,links[0].user)==KILN_TYPED_CONTINUE); CHECK(originals==0); }
    if(!denied) {
        KilnCallFrame frame{}; frame.slot=1; uint64_t stack[2]{}; frame.stack=stack;
        CHECK(links[0].fn(&frame,links[0].user)==KILN_TYPED_SKIP); CHECK(originals==1); CHECK(frame.rax==7);
    }
    std::printf("remaining %s: %u checks, %u failures\n",mode.c_str(),checks,failed); return failed ? 1 : 0;
}
