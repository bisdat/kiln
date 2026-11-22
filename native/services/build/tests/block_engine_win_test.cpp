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
LONG lock_count=0;int releases=0;bool ready=true,uniform=true,found=true,decode_ok=true;
uint8_t material=2,fill=128;uint8_t packed[16]{};
template<class T>void put(uint8_t* p,int off,T v){std::memcpy(p+off,&v,sizeof v);}
uint8_t* acquire(uint8_t* h,uintptr_t,uint32_t,uint64_t){
    InterlockedIncrement(&lock_count);put(h,0,reinterpret_cast<uintptr_t>(&lock_count));put(h,8,uint32_t{1});
    put(h,0x10,uintptr_t{123});put(h,0x18,uint32_t{7});put(h,0x20,uintptr_t{456});h[0x90]=ready;return h;
}
uint8_t seek(uint8_t* h,uint32_t){h-=0x30;h[0x80]=uniform;put(h,0x68,&material);put(h,0x70,&fill);
    put(h,0x50,packed);put(h,0x60,uint32_t{16});put(h,0x88,uint32_t{16});return found;
}
struct Span {void* data;uint64_t count;};
void* decode(uint8_t* result,Span* out,Span*){auto* p=static_cast<uint16_t*>(out->data);for(unsigned i=0;i<512;++i)p[i]=static_cast<uint16_t>(i);result[0]=decode_ok?0:1;return result;}
void release(uintptr_t loader,uint32_t id){if(loader==123 && id==7)++releases;}
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
    kb::BlockBaseFunctions f{reinterpret_cast<uintptr_t>(&acquire),reinterpret_cast<uintptr_t>(&seek),reinterpret_cast<uintptr_t>(&decode),reinterpret_cast<uintptr_t>(&release)};
    int32_t brick[3]={1,2,3};uint16_t voxels[512]{};
    check(kb::block_base_brick(f,123,brick,voxels)==1 && voxels[0]==0x8002 && voxels[511]==0x8002,"uniform baked brick copied");
    check(lock_count==0 && releases==1,"loader reference and world lock both released");
    ready=false;check(kb::block_base_brick(f,123,brick,voxels)==0 && lock_count==0 && releases==2,"unready column unknown with balanced release");ready=true;
    found=false;check(kb::block_base_brick(f,123,brick,voxels)==1 && voxels[0]==0 && lock_count==0,"missing brick in loaded column is air");found=true;
    uniform=false;check(kb::block_base_brick(f,123,brick,voxels)==1 && voxels[511]==511,"compressed baked brick decode");
    decode_ok=false;check(kb::block_base_brick(f,123,brick,voxels)==0 && lock_count==0,"decoder failure unknown with balanced release");decode_ok=true;
    brick[1]=-1;check(kb::block_base_brick(f,123,brick,voxels)==1 && voxels[0]==0xff01,"negative y sentinel");brick[1]=2;
    brick[0]=-1;check(kb::block_base_brick(f,123,brick,voxels)==0,"negative x unknown");brick[0]=1;
    std::printf("block engine: %d passed, %d failed\n",total-failed,failed);return failed?1:0;
}
