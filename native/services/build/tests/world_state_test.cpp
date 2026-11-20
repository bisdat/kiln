// SPDX-License-Identifier: MIT
#include "../../../core/abi/kiln_api.h"
#include "../../../core/abi/services/kiln_spawn.h"
#include <cstdio>
namespace kb {
uint64_t g_steps=10000; // an earlier hosted world already ran for much longer than four minutes
uintptr_t live=0;
bool observed=true, save_loaded=false;
uint32_t spawn_state=KILN_SPAWN_WORLD_LOADING;
KilnResult session(KilnSessionInfo* s) { s->lifecycle=observed; s->worlds[KILN_WORLD_SERVER]=live; s->state=live?KILN_SESSION_HOST:KILN_SESSION_NONE; return KILN_OK; }
uint32_t state() {return spawn_state;}
KilnApi api{};
KilnApi* g_api=&api;
KilnSpawnApi spawn{};
KilnSpawnApi* g_spawn=nullptr;
uintptr_t server_world(){return live;}
bool regions_lifecycle_safe(){return observed;}
bool regions_save_running(){return save_loaded;}
void set_detail(const char*){}
}
#define KILN_BUILD_WORLD_TEST
#include "../build.cpp"
int main(){
    using namespace kb;
    api.session=&session; spawn.world_state=&state;
    int fail=0,total=0;
    auto check=[&](bool ok,const char* name){++total;if(!ok){++fail;std::fprintf(stderr,"FAIL %s\n",name);}};
    check(!world_running(),"old process steps do not make a missing world run");
    live=42;
    check(!world_running(),"first world step while save loads is not Run");
    save_loaded=true;
    check(world_running(),"core live world plus confirmed save load runs without Spawn");
    g_spawn=&spawn;
    check(!world_running(),"Spawn loading is authoritative");
    spawn_state=KILN_SPAWN_WORLD_RUNNING;
    check(world_running(),"hosted world Run");
    live=0;
    check(!world_running(),"hosted stop refuses even with stale Spawn state");
    live=43; observed=false;
    check(!world_running(),"no lifecycle observation refuses writes");
    observed=true; api.session=nullptr; g_spawn=nullptr;
    check(!world_running(),"no usable world-state source refuses");
    std::printf("world state: %d passed, %d failed\n",total-fail,fail);
    return fail?1:0;
}
