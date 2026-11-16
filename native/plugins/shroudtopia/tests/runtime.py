#!/usr/bin/env python3
"""Compile the production shim/Win32 adapter with host I/O mocks, then call it through a synthetic mod's IAT."""
import os
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[4]
source = (root/'native/plugins/shroudtopia/src/original_memcmp.cpp').read_text()
production = source[source.index('namespace kshroud::original {'):]
out = Path(os.environ['KILN_OUT'])/'original-runtime'
out.mkdir(parents=True, exist_ok=True)
preamble = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include "original_code.h"
#include "memcmp_import.h"
#include "original_code_service.h"
#define __cdecl
using DWORD=uint32_t;using DWORD64=uint64_t;using SIZE_T=size_t;using HMODULE=void*;
constexpr unsigned IMAGE_DOS_SIGNATURE=0x5A4D,IMAGE_NT_SIGNATURE=0x4550,IMAGE_FILE_MACHINE_AMD64=0x8664,
 IMAGE_NT_OPTIONAL_HDR64_MAGIC=0x20B,IMAGE_SCN_MEM_EXECUTE=0x20000000,PAGE_READWRITE=4;
#pragma pack(push,1)
struct IMAGE_DOS_HEADER {uint16_t e_magic;uint8_t unused[58];int32_t e_lfanew;};
struct IMAGE_FILE_HEADER {uint16_t Machine,NumberOfSections;uint32_t TimeDateStamp;uint8_t unused[8];uint16_t SizeOfOptionalHeader,Characteristics;};
struct IMAGE_OPTIONAL_HEADER64 {uint16_t Magic;uint8_t unused[54];uint32_t SizeOfImage;uint8_t rest[180];};
struct IMAGE_NT_HEADERS64 {uint32_t Signature;IMAGE_FILE_HEADER FileHeader;IMAGE_OPTIONAL_HEADER64 OptionalHeader;};
struct IMAGE_SECTION_HEADER {uint8_t Name[8];union {uint32_t VirtualSize;}Misc;uint32_t VirtualAddress,SizeOfRawData,PointerToRawData;uint8_t rest[12];uint32_t Characteristics;};
#pragma pack(pop)
struct MODULEINFO {void* lpBaseOfDll;DWORD SizeOfImage;};
struct RUNTIME_FUNCTION {uint32_t BeginAddress,EndAddress,UnwindData;};
std::vector<uint8_t> game,mod;
std::filesystem::path exe;
RUNTIME_FUNCTION function_entry{0x1040,0x10C0,0};
bool protect_fail=false;
unsigned protection_calls=0,used_calls=0;
uint32_t used_rva=0;
void* GetCurrentProcess(){return nullptr;}
HMODULE GetModuleHandleW(const wchar_t*){return game.data();}
DWORD GetModuleFileNameW(void*,wchar_t* out,DWORD cap){auto path=exe.wstring();if(path.size()>=cap)return cap;std::copy(path.begin(),path.end(),out);return path.size();}
bool ReadProcessMemory(void*,const void* from,void* to,size_t n,size_t* got){
 auto within=[&](const void* p,size_t size){auto at=reinterpret_cast<uintptr_t>(from),base=reinterpret_cast<uintptr_t>(p);return at>=base && at-base<=size && n<=size-(at-base);};
 if(!within(game.data(),game.size()) && !within(mod.data(),mod.size()) && !within(&function_entry,sizeof function_entry))return false;
 std::memcpy(to,from,n);*got=n;return true;
}
bool K32GetModuleInformation(void*,HMODULE h,MODULEINFO* out,size_t){if(h!=mod.data())return false;*out={mod.data(),DWORD(mod.size())};return true;}
bool VirtualProtect(void*,size_t,DWORD,DWORD* old){++protection_calls;*old=2;return !protect_fail;}
void* InterlockedExchangePointer(void* volatile* at,void* value){void* old;std::memcpy(&old,const_cast<void**>(at),8);std::memcpy(const_cast<void**>(at),&value,8);return old;}
RUNTIME_FUNCTION* RtlLookupFunctionEntry(uint64_t,uint64_t* base,void*){*base=reinterpret_cast<uintptr_t>(game.data());return &function_entry;}
'''
main = r'''
int checks=0,failures=0;
void check(bool b,const char* label){++checks;if(!b){++failures;std::cerr<<"FAIL "<<label<<"\n";}}
template<class T>void put(std::vector<uint8_t>& b,size_t at,T value){std::memcpy(b.data()+at,&value,sizeof value);}
int fallback(const void* a,const void* b,size_t n){return std::memcmp(a,b,n);}
int main(int,char** argv){
 const std::string scenario=argv[1];exe=std::filesystem::path(argv[2])/"game-image.bin";
 std::vector<uint8_t>file(0x500,0);game.resize(0x2000);mod.resize(0x1000);
 const uintptr_t base=reinterpret_cast<uintptr_t>(game.data());
 put<uint16_t>(file,0,0x5A4D);put<uint32_t>(file,0x3C,0x40);put<uint32_t>(file,0x40,0x4550);
 put<uint16_t>(file,0x44,0x8664);put<uint16_t>(file,0x46,1);put<uint32_t>(file,0x48,42);put<uint16_t>(file,0x54,0xF0);
 put<uint16_t>(file,0x58,0x20B);put<uint64_t>(file,0x58+24,base);put<uint32_t>(file,0x58+56,game.size());put<uint32_t>(file,0x58+60,0x400);put<uint32_t>(file,0x58+108,16);
 std::memcpy(file.data()+0x148,".text",5);put<uint32_t>(file,0x150,0x100);put<uint32_t>(file,0x154,0x1000);put<uint32_t>(file,0x158,0x100);put<uint32_t>(file,0x15C,0x400);
 put<uint32_t>(file,0x148+36,scenario=="not-executable"?0x40000040:0x60000020);
 std::fill(file.begin()+0x400,file.end(),0xCC);
 const uint8_t prologue[]={0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x55,0x57,0x41,0x55,0x41,0x56};
 std::memcpy(file.data()+0x440,prologue,sizeof prologue);
 std::memcpy(game.data(),file.data(),0x400);std::memcpy(game.data()+0x1000,file.data()+0x400,0x100);
 std::array<uint8_t,32> signature;std::memcpy(signature.data(),game.data()+0x1040,32);
 std::ofstream f(exe,std::ios::binary);f.write(reinterpret_cast<const char*>(file.data()),file.size());f.close();
 if(scenario=="unreadable")exe+=".missing";
 if(scenario=="mismatch")put<uint32_t>(game,0x48,43);
 if(scenario!="no-hook") {game[0x1040]=0xE9;put<int32_t>(game,0x1041,0x4000-(0x1040+5));}
 if(scenario=="suffix")game[0x1058]^=1;
 if(scenario=="not-entry")function_entry.BeginAddress=0x103F;
 if(scenario=="protect-failure")protect_fail=true;
 put<uint16_t>(mod,0,0x5A4D);put<uint32_t>(mod,0x3C,0x80);put<uint32_t>(mod,0x80,0x4550);put<uint16_t>(mod,0x84,0x8664);put<uint16_t>(mod,0x94,0xF0);put<uint16_t>(mod,0x98,0x20B);
 put<uint32_t>(mod,0x98+56,mod.size());put<uint32_t>(mod,0x98+108,16);put<uint32_t>(mod,0x98+120,0x200);put<uint32_t>(mod,0x98+124,40);
 put<uint32_t>(mod,0x200,0x300);put<uint32_t>(mod,0x20C,0x400);put<uint32_t>(mod,0x210,0x500);std::memcpy(mod.data()+0x400,"VCRUNTIME140.dll",16);
 put<uint64_t>(mod,0x300,0x600);std::memcpy(mod.data()+0x602,scenario=="no-import"?"memcpy":"memcmp",7);put<uintptr_t>(mod,0x500,reinterpret_cast<uintptr_t>(&fallback));
 const kiln::compat::OriginalCodeApi rules{{sizeof(kiln::compat::OriginalCodeApi),1},nullptr,[](uint32_t rva){++used_calls;used_rva=rva;}};
 const auto before_game=game;
 const size_t patched=kshroud::original::install(mod.data(),&rules);
 const bool installed=scenario!="unreadable" && scenario!="mismatch" && scenario!="not-executable" && scenario!="no-import" && scenario!="protect-failure";
 check(patched==size_t(installed),"production adapter patches exactly when the original file and import are usable");
 using Compare=int(*)(const void*,const void*,size_t);Compare compare;std::memcpy(&compare,mod.data()+0x500,8);
 const bool answer=installed && scenario!="suffix" && scenario!="not-entry" && scenario!="no-hook";
 const int result=compare(game.data()+0x1040,signature.data(),signature.size());
 check((result==0)==(answer || scenario=="no-hook"),"IAT call preserves or substitutes the comparison correctly");
 check(compare(signature.data(),game.data()+0x1040,signature.size())==-result,"both game-operand positions work");
 check(used_calls==unsigned(answer),"first original-code answer logs once");
 if(answer){check(used_rva==0x1040,"first-use log reports game rva");check(compare(game.data()+0x1041,signature.data()+1,2)==0,"partial jump operands compare through the real shim");}
 const uint8_t high[]={0xFF},low[]={0x01};check(compare(high,low,1)>0 && compare(low,high,1)<0,"outside-code comparisons retain unsigned memcmp ordering");
 check(compare(high,high,0)==0,"zero-byte comparison is unchanged");
 check(game==before_game,"neither install nor compare changes game code");
 std::cout<<scenario<<": "<<checks-failures<<"/"<<checks<<" checks passed\n";return failures?1:0;
}
'''
cpp=out/'test.cpp'
cpp.write_text(preamble+production+main)
subprocess.run(['c++','-std=c++20','-O1','-g','-Wall','-Wextra','-fsanitize=address,undefined',
    '-I'+str(root/'native/plugins/shroudtopia/src'),'-I'+str(root/'native/plugins/compat'),str(cpp),'-o',str(out/'test')],check=True)
for scenario in ['hook','no-hook','suffix','not-entry','unreadable','mismatch','not-executable','no-import','protect-failure']:
    subprocess.run([str(out/'test'),scenario,str(out)],check=True)
