// SPDX-License-Identifier: MIT
// Real service hooks with fake kiln.hooks and session tables; no game/Windows execution.
#include "../hooks.cpp"
#include "../zones.cpp"
#include <cstdio>
#include <thread>
namespace kg {
const KilnApi* g_api; KilnOwner g_owner=1; Layout L; Fns fn; Features F; EventIds H; Stats S; uint64_t g_step;
void logf(int,const char*,...) {}
bool has_subscribers(uint32_t) { return true; }
bool entity_position(KilnEntity,KilnVec3*) { return false; }
bool seh_call(void(*f)(void*),void* p,unsigned long*) { f(p); return true; }
bool seh_build_target(uintptr_t);
bool dispatch(KilnGameplayEvent*) { return false; }
}
static int installs, calls, fail_at, failures, checks; static bool live; static uint32_t other_worlds; static bool valid=true;
namespace kg { bool seh_build_target(uintptr_t) { return valid; } }
static uint8_t original(uint8_t*,void*,uint32_t) { ++calls; return 7; }
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#x); } } while(0)
int main(int argc,char** argv) {
    const std::string mode=argc>1 ? argv[1] : "normal";
    KilnHooksApi hooks{}; hooks.header={sizeof hooks,2};
    hooks.raw_detour=[](KilnOwner,uintptr_t,const KilnHookOrder*,void*,void** orig,KilnHandle* out)->KilnResult {
        ++installs; if(installs==fail_at) return KILN_ERR_UNAVAILABLE;
        *orig=reinterpret_cast<void*>(&original); *out=installs; return KILN_OK;
    };
    KilnApi api{}; api.size=sizeof api; api.service=[](KilnStr,uint32_t)->const void* { return nullptr; };
    // Store the fake service via a static, matching the real ABI.
    static const KilnHooksApi* table; table=&hooks;
    api.service=[](KilnStr s,uint32_t)->const void* { return std::string_view(s.ptr,s.len)=="kiln.hooks" ? table : nullptr; };
    api.session=[](KilnSessionInfo* s)->KilnResult { s->worlds[0]=live ? 1 : 0;
        for(unsigned k=1;k<KILN_WORLD_KINDS;++k) { s->worlds[k]=(other_worlds & (1u<<k)) ? 1 : 0; }
        s->lifecycle=1; return KILN_OK; };
    api.symbol=[](KilnStr,uintptr_t* p)->KilnResult { *p=reinterpret_cast<uintptr_t>(&original); return KILN_OK; };
    hooks.typed_install_ready=[]()->uint32_t { return live || other_worlds ? 0 : 2; };
    hooks.detour_typed_checked=[](KilnOwner,uintptr_t,const KilnHookOrder*,const KilnSig*,KilnTypedFn,void*,KilnHandle* out)->KilnResult { *out=100; return KILN_OK; };
    hooks.call_original_frame=[](KilnCallFrame*)->KilnResult { return KILN_OK; };
    api.feature=[](KilnStr)->bool { return true; };
    kg::g_api=&api; kg::F.build=true; kg::F.craft=kg::F.terraform=kg::F.spawn=true;
    kg::fn.craft=kg::fn.terraform=kg::fn.queue_spawn=reinterpret_cast<uintptr_t>(&original);
    kg::fn.place_block=kg::fn.place_prop=kg::fn.dismantle=kg::fn.dismantle2=kg::fn.pick_up=reinterpret_cast<uintptr_t>(&original);
    kg::hooks_install(); CHECK(installs==0);
    CHECK(reinterpret_cast<uint8_t(*)(uint8_t*,void*,uint32_t)>(kg::fn.place_prop)(nullptr,nullptr,1)==7);
    calls=0;
#ifdef KILN_BUILD_BASELINE
    return failures ? 1 : 0;
#else
    kg::build_lifecycle(mode!="unobserved");
    if(mode=="failure") fail_at=3;
    if(mode=="precheck") valid=false;
    if(mode=="zones") CHECK(kg::hooks_request(KILN_GAMEPLAY_HOOK_TERRAFORM,11));
    live=mode!="immediate";
    if(mode=="hosted") other_worlds=6;
    std::thread a([] { kg::build_request(11); }), b([] { kg::build_request(22); }); a.join(); b.join();
    KilnGameplayZone zone{}; zone.size=sizeof zone; zone.shape=KILN_GAMEPLAY_ZONE_SPHERE; zone.radius=10;
    uint64_t allowed=0; zone.allowed_steam_ids=&allowed;
    uint32_t id=0;
    if(mode=="zones") {
        zone.deny=KILN_GAMEPLAY_DENY_BUILD;
        CHECK(kg::zone_add(11,&zone,&id)==KILN_ERR_UNAVAILABLE); CHECK(id==0); CHECK(kg::zone_count()==0);
        zone.deny=KILN_GAMEPLAY_DENY_TERRAFORM; CHECK(kg::zone_add(11,&zone,&id)==KILN_OK);
        zone.deny=KILN_GAMEPLAY_DENY_BREAK;
        CHECK(kg::zone_update(id,&zone)==KILN_ERR_UNAVAILABLE); CHECK(!kg::zones_any(KILN_GAMEPLAY_DENY_BREAK));
    }
    if(live) { CHECK(installs==0); CHECK(!kg::build_installed());
        kg::build_poll(); CHECK(installs==0); }
    if(mode=="hosted") {
        kg::build_poll(KILN_WORLD_SERVER); CHECK(installs==0);
        live=false; kg::build_poll(KILN_WORLD_PREDICTION); CHECK(installs==0);
        other_worlds=4; kg::build_poll(KILN_WORLD_CLIENT);
    }
    if(live) kg::build_poll(KILN_WORLD_SERVER);
    live=false; kg::build_poll();
    if(mode=="precheck" || mode=="unobserved") {
        CHECK(installs==0); CHECK(!kg::build_installed()); CHECK(kg::build_state()==3);
        CHECK(!kg::build_reason().empty());
    } else if(fail_at) {
        CHECK(installs==3); CHECK(!kg::build_installed());
        CHECK(kg::build_state()==3); CHECK(kg::build_mask()==3);
        CHECK(kg::hk_place_prop(nullptr,nullptr,1)==7); CHECK(calls==1); CHECK(kg::g_recs.empty());
    } else {
        CHECK(installs==5); CHECK(kg::build_installed()); CHECK(kg::build_state()==1);
    }
    if(mode=="zones") {
        CHECK(kg::zone_update(id,&zone)==KILN_OK); CHECK(kg::zones_any(KILN_GAMEPLAY_DENY_BREAK));
        zone.deny=KILN_GAMEPLAY_DENY_BUILD; CHECK(kg::zone_add(11,&zone,&id)==KILN_OK); CHECK(kg::zone_count()==2);
    }
    kg::build_request(33); kg::build_poll(); CHECK(installs==((mode=="precheck" || mode=="unobserved") ? 0 : fail_at ? 3 : 5));
    std::printf("build_hooks_test: %d checks, %d failures\n",checks,failures); return failures ? 1 : 0;
#endif
}
