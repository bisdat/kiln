// SPDX-License-Identifier: MIT
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#if __has_include("../src/memcmp_import.h")
#include "../src/memcmp_import.h"
#else
namespace kshroud::original {template<class Read,class Write> size_t patch_memcmp(uint8_t*,size_t,uintptr_t,Read,Write){return 0;}}
#endif
int failures=0,checks=0;
void check(bool ok,const char* s){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<s<<"\n";}}
template<class T>void put(std::vector<uint8_t>& b,size_t at,T v){std::memcpy(b.data()+at,&v,sizeof v);}
uint64_t get(const std::vector<uint8_t>& b,size_t at){uint64_t v;std::memcpy(&v,b.data()+at,8);return v;}
std::vector<uint8_t> fixture(const char* dll){
 std::vector<uint8_t>b(4096);put<uint16_t>(b,0,0x5A4D);put<uint32_t>(b,0x3C,0x80);put<uint32_t>(b,0x80,0x4550);
 put<uint16_t>(b,0x84,0x8664);put<uint16_t>(b,0x94,0xF0);put<uint16_t>(b,0x98,0x20B);
 put<uint32_t>(b,0x98+56,b.size());put<uint32_t>(b,0x98+108,16);
 put<uint32_t>(b,0x98+120,0x200);put<uint32_t>(b,0x98+124,40);
 put<uint32_t>(b,0x200,0x300);put<uint32_t>(b,0x20C,0x400);put<uint32_t>(b,0x210,0x500);
 std::strcpy(reinterpret_cast<char*>(b.data()+0x400),dll);
 put<uint64_t>(b,0x300,0x600);put<uint64_t>(b,0x308,0x620);put<uint64_t>(b,0x310,0x800000000000003B);
 std::strcpy(reinterpret_cast<char*>(b.data()+0x602),"memcmp");std::strcpy(reinterpret_cast<char*>(b.data()+0x622),"memcpy");
 put<uint64_t>(b,0x500,0x1111);put<uint64_t>(b,0x508,0x2222);put<uint64_t>(b,0x510,0x3333);return b;
}
int main(){
 auto read=[](void* to,const void* from,size_t n){std::memcpy(to,from,n);return true;};
 auto write=[](void* slot,uintptr_t value){std::memcpy(slot,&value,8);return true;};
 for(const char* dll:{"VCRUNTIME140.dll","ucrtbase.dll","api-ms-win-crt-string-l1-1-0.dll"}){
  auto b=fixture(dll),before=b;auto count=kshroud::original::patch_memcmp(b.data(),b.size(),0xABCD,read,write);
  check(count==1 && get(b,0x500)==0xABCD,"patches memcmp regardless of CRT DLL name");
  put<uint64_t>(before,0x500,0xABCD);check(b==before,"only this DLL's memcmp IAT slot changes");
 }
 auto patch=[&](std::vector<uint8_t>& b){return kshroud::original::patch_memcmp(b.data(),b.size(),0xABCD,read,write);};
 auto b=fixture("crt.dll");std::strcpy(reinterpret_cast<char*>(b.data()+0x602),"memcmp_extra");auto before=b;
 check(patch(b)==0 && b==before,"no exact memcmp import makes no change");
 b=fixture("crt.dll");put<uint32_t>(b,0x200,0);before=b;check(patch(b)==0 && b==before,"missing name table is left alone");
 b=fixture("crt.dll");put<uint64_t>(b,0x300,0xFFFFFFFE);before=b;check(patch(b)==0 && b==before,"bad import name RVA makes no change");
 b=fixture("crt.dll");put<uint32_t>(b,0x210,4092);before=b;check(patch(b)==0 && b==before,"truncated IAT makes no change");
 b=fixture("crt.dll");put<uint32_t>(b,0x210,0x501);before=b;check(patch(b)==0 && b==before,"unaligned IAT makes no change");
 b=fixture("crt.dll");put<uint32_t>(b,0x98+124,20);before=b;check(patch(b)==0 && b==before,"unterminated import directory makes no change");
 b=fixture("crt.dll");put<uint32_t>(b,0x3C,0xFFFFFFFE);before=b;check(patch(b)==0 && b==before,"overflowing PE offset makes no change");
 b=fixture("crt.dll");before=b;
 check(kshroud::original::patch_memcmp(b.data(),b.size(),0xABCD,read,[](void*,uintptr_t){return false;})==0 && b==before,"protection failure makes no change");
 check(kshroud::original::patch_memcmp(b.data(),b.size(),0xABCD,[](void*,const void*,size_t){return false;},write)==0,"unreadable headers make no change");
 std::cout<<checks-failures<<"/"<<checks<<" checks passed\n";return failures?1:0;
}
