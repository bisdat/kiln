// SPDX-License-Identifier: MIT
// Exercise the shared writer's permission guards without a game or native engine memory.
#include "../internal.h"
#include <cstdio>
namespace kb {
const KilnApi* g_api=nullptr;
const KilnGameplayApi* g_gameplay=nullptr;
Features F;
Layout L;
uintptr_t server_world(){return 123;}
bool tick_thread(){return true;}
bool seh_copy(void*,const void*,size_t){return false;}
void set_detail(const char*,...){}
}
namespace {
uintptr_t KILN_CALL singleton(uintptr_t,KilnStr){return 456;}
}
int main(){
    int total=0,failed=0;
    auto check=[&](bool ok,const char* name){++total;if(!ok){++failed;std::printf("FAIL %s\n",name);}};
    KilnApi api{};api.singleton=&singleton;kb::g_api=&api;
    const KilnVec3 lo{0,0,0},hi{1,1,1};KilnEntity altar=0;
    check(kb::touches_base(lo,hi,&altar),"unqualified base mapping conservatively refuses edits");
    kb::F.zones=true;
    check(kb::touches_base(lo,hi,&altar),"unreadable base list conservatively refuses edits");
    check(kb::zone_denies_box(42,lo,hi,KILN_GAMEPLAY_DENY_BUILD)!=0,"RespectZones cannot silently allow a missing gameplay service");
    KilnGameplayApi gameplay{};gameplay.header.size=sizeof gameplay;kb::g_gameplay=&gameplay;
    check(kb::zone_denies_box(42,lo,hi,KILN_GAMEPLAY_DENY_BUILD)!=0,"RespectZones cannot silently allow a missing zone check");
    check(kb::zone_denies_box(42,lo,hi,0)==0,"empty action does not require a zone checker");
    std::printf("block policy: %d passed, %d failed\n",total-failed,failed);return failed?1:0;
}
