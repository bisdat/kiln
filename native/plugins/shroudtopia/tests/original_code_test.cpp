// SPDX-License-Identifier: MIT
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#if __has_include("../src/original_code.h")
#include "../src/original_code.h"
#else
namespace kshroud::original {
struct View {
 uintptr_t base=0; uint32_t image_size=0,text_rva=0;std::vector<uint8_t> bytes;
 template<class Copy> const uint8_t* range(uintptr_t,size_t,uintptr_t,uintptr_t,Copy,uintptr_t*)const{return nullptr;}
};
}
#endif
using kshroud::original::View;
int checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<label<<"\n";}}
int main(){
 std::vector<uint8_t> live(256,0xCC); View v;
 v.base=reinterpret_cast<uintptr_t>(live.data())-0x1000;v.image_size=0x4000;v.text_rva=0x1000;
 v.bytes=live;const size_t entry=64;
 const uint8_t prologue[]={0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x55,0x57,0x41,0x55,0x41,0x56};
 std::memcpy(v.bytes.data()+entry,prologue,sizeof prologue);
 const uintptr_t at=reinterpret_cast<uintptr_t>(live.data()+entry),outside=v.base+0x8000;
 auto copy=[](void* to,const void* from,size_t n){std::memcpy(to,from,n);return true;};
 auto range=[&](size_t offset,size_t count){uintptr_t target=0;return v.range(at+offset,count,at,at+128,copy,&target);};
 auto hook=[&](int shape){
  live=v.bytes;auto p=live.data()+entry;
  if(shape==0){p[0]=0xE9;int32_t rel=static_cast<int32_t>(outside-(at+5));std::memcpy(p+1,&rel,4);}
  if(shape==1){p[0]=0xFF;p[1]=0x25;std::memset(p+2,0,4);std::memcpy(p+6,&outside,8);}
  if(shape==2){p[0]=0x48;p[1]=0xB8;std::memcpy(p+2,&outside,8);p[10]=0xFF;p[11]=0xE0;}
  if(shape==3){p[0]=0x49;p[1]=0xBB;std::memcpy(p+2,&outside,8);p[10]=0x41;p[11]=0xFF;p[12]=0xE3;}
 };
 live=v.bytes;check(!range(0,32),"unhooked code uses ordinary memcmp");
 for(int shape=0;shape<4;++shape){
  hook(shape);check(range(0,32)==v.bytes.data()+entry,"recognised entry hook uses original code");
  check(range(1,2)==v.bytes.data()+entry+1,"partial overlap inside jump uses original code");
  check(!range(40,8),"range after hook uses ordinary memcmp");
  live[entry+24]^=1;check(!range(0,32),"an additional in-place patch keeps live bytes");
 }
 hook(0);
 auto original=range(0,32);
 check(original && std::memcmp(original,v.bytes.data()+entry,32)==0,"hooked game operand compares equal to original signature");
 auto expected=v.bytes;expected[entry+31]=0xCD;
 check(original && std::memcmp(original,expected.data()+entry,32)<0 && std::memcmp(expected.data()+entry,original,32)>0,
       "memcmp preserves unsigned ordering in either argument position");
 check(!range(0,4097),"large comparisons keep ordinary memcmp");
 hook(0);live[entry+5]=0x90;check(range(0,32),"hook instruction padding is recognised");
 hook(0);live[entry+8]=0xEB;check(!range(0,32),"non-NOP changed suffix is not hidden");
 hook(0);int32_t inside=16;std::memcpy(live.data()+entry+1,&inside,4);check(!range(0,32),"jump inside game is not an entry hook");
 hook(0);uintptr_t target=0;
 check(!v.range(at,32,at-1,at+128,copy,&target),"mid-function jump is not a function entry");
 check(!v.range(at,32,at,at+4,copy,&target),"truncated function does not read a partial hook");
 check(!v.range(at,129,at,at+128,copy,&target),"comparison crossing function boundary uses live bytes");
 check(!v.range(v.base+0x500,8,v.base+0x500,v.base+0x600,copy,&target),"range outside executable code uses live bytes");
 check(!v.range(at,SIZE_MAX,at,at+128,copy,&target),"overflowing comparison uses live bytes");
 check(!v.range(at,0,at,at+128,copy,&target),"zero-byte comparison uses ordinary memcmp");
 check(!v.range(at,32,at,at+128,[](void*,const void*,size_t){return false;},&target),"unreadable live code uses ordinary memcmp");
 View unavailable;check(!unavailable.range(at,32,at,at+128,copy,&target),"unreadable exe file leaves original view unavailable");
 std::cout<<checks-failures<<"/"<<checks<<" checks passed\n";return failures?1:0;
}
