// SPDX-License-Identifier: MIT
// Qualified material lookup calls and copies, isolated from C++ destructors for Windows SEH.
#include <windows.h>
#include <cstring>
#include "block_runtime.h"
namespace kb {
bool block_config_read(uintptr_t function,uintptr_t registry,uint8_t id,int64_t hardness_offset,
                       int64_t health_offset,int32_t* hardness,int32_t* health) {
    if(!function || !registry || !hardness || !health || hardness_offset!=0x14 || health_offset!=0x1c)return false;
    using Lookup=const uint8_t* (*)(uintptr_t,uint8_t);
    __try {
        const auto* cfg=reinterpret_cast<Lookup>(function)(registry,id);
        if(!cfg)return false;
        const uint8_t h=cfg[hardness_offset];int32_t hp=0;
        std::memcpy(&hp,cfg+health_offset,sizeof hp);
        if(h>5 || hp<0)return false;
        *hardness=h;*health=hp;return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
