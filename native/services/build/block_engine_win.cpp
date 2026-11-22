// SPDX-License-Identifier: MIT
// Qualified material lookup calls and copies, isolated from C++ destructors for Windows SEH.
#include <windows.h>
#include <cstring>
#include "block_runtime.h"
namespace kb {
namespace {
template<class T> T field(const uint8_t* h,size_t offset) { T v{};std::memcpy(&v,h+offset,sizeof v);return v; }
struct BaseSpan {void* data;uint64_t count;};
}
int block_base_brick(const BlockBaseFunctions& f,uintptr_t editor,const int32_t brick[3],KilnVoxel out[512]) {
    if(!editor || !brick || !out || !f.acquire || !f.seek || !f.decode || !f.release || brick[0]<0 || brick[2]<0)return 0;
    if(brick[1]<0){for(size_t i=0;i<512;++i)out[i]=0xff01;return 1;}
    alignas(16) uint8_t h[0xa0]{};bool acquired=false;int result=0;
    using Acquire=void* (*)(void*,uintptr_t,uint32_t,uint64_t);
    using Seek=uint8_t (*)(void*,uint32_t);
    using Decode=void* (*)(void*,BaseSpan*,BaseSpan*);
    using Release=void (*)(uintptr_t,uint32_t);
    __try {
        __try {
            reinterpret_cast<Acquire>(f.acquire)(h,editor,6,static_cast<uint32_t>(brick[0]) | (static_cast<uint64_t>(static_cast<uint32_t>(brick[2]))<<32));
            acquired=true;
            if(h[0x90] && field<uintptr_t>(h,0x20)) {
                if(!reinterpret_cast<Seek>(f.seek)(h+0x30,static_cast<uint32_t>(brick[1]))) {
                    std::memset(out,0,512*sizeof *out);result=1;
                }else if(h[0x80]) {
                    const auto* mp=field<const uint8_t*>(h,0x68);const auto* fp=field<const uint8_t*>(h,0x70);
                    if(mp){const uint8_t m=*mp,fill=fp?*fp:(m?255:0);const auto v=KILN_VOXEL_MAKE(m,fill);
                        for(size_t i=0;i<512;++i)out[i]=v;result=1;}
                }else {
                    auto* begin=field<uint8_t*>(h,0x50);const auto end=field<uint32_t>(h,0x60),size=field<uint32_t>(h,0x88);
                    if(begin && size && size<=end) {
                        BaseSpan dst{out,512},src{begin+end-size,size};uint8_t decoded[8]{};
                        reinterpret_cast<Decode>(f.decode)(decoded,&dst,&src);result=decoded[0]==0?1:0;
                    }
                }
            }
        }__except(EXCEPTION_EXECUTE_HANDLER){result=-1;}
    }__finally {
        // A returned acquire owns both a loader reference and a world lock. Release in that order, even when not ready.
        // A fault INSIDE acquire has no proven loader unwind: do not pretend published bytes prove full ownership.
        if(acquired) {
            __try {
                if(field<uintptr_t>(h,0x20))reinterpret_cast<Release>(f.release)(field<uintptr_t>(h,0x10),field<uint32_t>(h,0x18));
            }__except(EXCEPTION_EXECUTE_HANDLER){result=-1;}
            __try {
                auto* lock=field<volatile LONG*>(h,0);const auto mode=field<uint32_t>(h,8);
                if(lock && mode==1)InterlockedDecrement(lock);
                else if(lock && mode==2)InterlockedExchangeAdd(lock,0x7fff0000);
            }__except(EXCEPTION_EXECUTE_HANDLER){result=-1;}
        }
    }
    return result;
}
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
