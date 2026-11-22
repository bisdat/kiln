// SPDX-License-Identifier: MIT
#include <windows.h>
#include "../block_runtime.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
namespace {
uint8_t cfg[0x40]{};uintptr_t seen_registry=0;uint8_t seen_id=0;
uintptr_t lookup(uintptr_t registry,uint8_t id){seen_registry=registry;seen_id=id;return reinterpret_cast<uintptr_t>(cfg);}
uintptr_t missing(uintptr_t,uint8_t){return 0;}
uintptr_t fault(uintptr_t,uint8_t){RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);return 0;}
}
int main(){
    int total=0,failed=0;auto check=[&](bool ok,const char* name){++total;if(!ok){++failed;std::printf("FAIL %s\n",name);}};
    int32_t h=-1,hp=-1,health=500;cfg[0x14]=4;std::memcpy(cfg+0x1c,&health,4);
    check(kb::block_config_read(reinterpret_cast<uintptr_t>(&lookup),123,15,0x14,0x1c,&h,&hp) && h==4 && hp==500,"metadata copied at qualified offsets");
    check(seen_registry==123 && seen_id==15,"native registry then material byte order");
    check(!kb::block_config_read(reinterpret_cast<uintptr_t>(&missing),123,1,0x14,0x1c,&h,&hp),"missing config safe");
    check(!kb::block_config_read(reinterpret_cast<uintptr_t>(&fault),123,1,0x14,0x1c,&h,&hp),"engine fault contained");
    cfg[0x14]=6;check(!kb::block_config_read(reinterpret_cast<uintptr_t>(&lookup),123,1,0x14,0x1c,&h,&hp),"invalid hardness rejected");
    cfg[0x14]=5;check(kb::block_config_read(reinterpret_cast<uintptr_t>(&lookup),123,1,0x14,0x1c,&h,&hp) && h==5,"Unbreakable is 5");
    health=-1;std::memcpy(cfg+0x1c,&health,4);check(!kb::block_config_read(reinterpret_cast<uintptr_t>(&lookup),123,1,0x14,0x1c,&h,&hp),"negative health rejected");
    check(!kb::block_config_read(0,123,1,0x14,0x1c,&h,&hp),"missing symbol rejected");
    check(!kb::block_config_read(reinterpret_cast<uintptr_t>(&lookup),0,1,0x14,0x1c,&h,&hp),"missing registry rejected");
    std::printf("block engine: %d passed, %d failed\n",total-failed,failed);return failed?1:0;
}
