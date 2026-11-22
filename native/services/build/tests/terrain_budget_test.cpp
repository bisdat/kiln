// SPDX-License-Identifier: MIT
#include "../terrain_budget.h"
#include "../../../core/abi/services/kiln_build.h"
#include <cstddef>
static_assert(offsetof(KilnBuildApi, terrain_budget_reserve) == 136);
static_assert(offsetof(KilnBuildApi, terrain_budget_charge) == 144);
static_assert(offsetof(KilnBuildApi, grid_for_session) == 152);
static_assert(sizeof(KilnBuildApi) == 184);
#include <cstdio>
int main() {
    kb::TerrainIoBudget b;
    int total=0,fail=0;
    auto check=[&](bool ok,const char* name){++total;if(!ok){++fail;std::fprintf(stderr,"FAIL %s\n",name);}};
    b.begin(1);
    check(b.acquire(2048,512),"native transient recovery reserves its work");
    for(int n=0;n<7;n++) check(b.acquire(2048,512),"overlay work fits the remaining shared allowance");
    check(!b.acquire(0,512),"recovery and overlay writes share 4096 cells");
    check(!b.acquire(32768,0),"recovery and overlay reads share 32768 cells");
    b.begin(2);
    b.charge(2);
    check(!b.acquire(0,0),"native elapsed time also stops managed admission");
    b.begin(3);
    check(b.acquire(32768,4096),"next step gets a fresh allowance");
    b.reset(); b.begin(3);
    check(b.acquire(0,512),"world teardown retires the old allowance even at a reused step");
    std::printf("shared native budget: %d passed, %d failed\n",total-fail,fail);
    return fail?1:0;
}
