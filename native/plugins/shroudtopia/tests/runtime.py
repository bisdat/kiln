#!/usr/bin/env python3
"""Compile the production shim/Win32 adapter with host I/O mocks, then call it through a synthetic mod's IAT."""
import os
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[4]
source = (root/'native/plugins/shroudtopia/src/original_memcmp.cpp').read_text()
production = source[source.index('namespace kshroud::original {'):]
core = (root/'native/core/src/compat.cpp').read_text()
core_read = core[core.index('bool KILN_CALL original_read('):core.index('const OriginalCodeApi kOriginalCodeTable')]
api = (root/'native/core/src/api.cpp').read_text()
lookup = api[api.index('const void* KILN_CALL api_service('):api.index('    // kiln.profile works')] + '    return nullptr;\n}\n'

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
#include <thread>
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
std::vector<uint8_t> game,mod,relay(32),disk;
std::string scenario;
unsigned warnings=0;
struct Host {template<class... T>void logf(T...) {++warnings;}} H;
std::filesystem::path exe;
RUNTIME_FUNCTION function_entry{0x1040,0x10C0,0};
bool protect_fail=false, restore_fail=false;
DWORD page_protection=0x20;
unsigned execute_removed=0,restore_failures=0;
struct MEMORY_BASIC_INFORMATION {void* BaseAddress;size_t RegionSize;DWORD State,Protect;};
constexpr DWORD MEM_COMMIT=0x1000,PAGE_EXECUTE=0x10,PAGE_EXECUTE_READ=0x20,PAGE_EXECUTE_READWRITE=0x40,PAGE_EXECUTE_WRITECOPY=0x80,PAGE_GUARD=0x100,PAGE_NOACCESS=1;
size_t VirtualQuery(const void* p,MEMORY_BASIC_INFORMATION* out,size_t){*out={const_cast<void*>(p),8,MEM_COMMIT,page_protection};return sizeof(*out);}
int publication_result=0;
unsigned protection_calls=0,used_calls=0;
uint32_t used_rva=0;
void* GetCurrentProcess(){return nullptr;}
HMODULE GetModuleHandleW(const wchar_t*){return game.data();}
DWORD GetModuleFileNameW(void*,wchar_t* out,DWORD cap){auto path=exe.wstring();if(path.size()>=cap)return cap;std::copy(path.begin(),path.end(),out);return path.size();}
bool ReadProcessMemory(void*,const void* from,void* to,size_t n,size_t* got){
 auto within=[&](const void* p,size_t size){auto at=reinterpret_cast<uintptr_t>(from),base=reinterpret_cast<uintptr_t>(p);return at>=base && at-base<=size && n<=size-(at-base);};
 if((scenario=="live-unreadable" && from==game.data()+0x1040 && n>=34) ||
    (scenario=="signature-unreadable" && from==mod.data()+0x700 && n==32))return false;
 if(scenario=="relay-unreadable" && reinterpret_cast<uintptr_t>(from)==reinterpret_cast<uintptr_t>(game.data())+0x4000)return false;
 if(reinterpret_cast<uintptr_t>(from)==reinterpret_cast<uintptr_t>(game.data())+0x4000 && n<=14){std::memset(to,0xCC,n);*got=n;return true;}
 if(!within(game.data(),game.size()) && !within(mod.data(),mod.size()) && !within(&function_entry,sizeof function_entry) && !within(relay.data(),relay.size()))return false;
 std::memcpy(to,from,n);*got=n;return true;
}
bool K32GetModuleInformation(void*,HMODULE h,MODULEINFO* out,size_t){if(h!=mod.data())return false;*out={mod.data(),DWORD(mod.size())};return true;}
bool VirtualProtect(void*,size_t,DWORD value,DWORD* old){
 ++protection_calls;*old=page_protection;
 if(protect_fail)return false;
 if(restore_fail && value==PAGE_EXECUTE_READ && (scenario=="restore-permanent" || !restore_failures++)){return false;}
 if((page_protection&0xF0) && !(value&0xF0))++execute_removed;
 page_protection=value;return true;
}
void* InterlockedExchangePointer(void* volatile* at,void* value){
 auto old=std::atomic_ref<void*>(*const_cast<void**>(at)).exchange(value);
 publication_result=reinterpret_cast<int(*)(const void*,const void*,size_t)>(value)(game.data()+0x1040,mod.data()+0x700,32);
 return old;
}
RUNTIME_FUNCTION* RtlLookupFunctionEntry(uint64_t,uint64_t* base,void*){*base=reinterpret_cast<uintptr_t>(game.data());return scenario=="missing-unwind"?nullptr:&function_entry;}
'''
core_adapter = r'''
#define __try try
#define __except(x) catch (...)
namespace kiln::sigdb {
bool read_fault=false;
unsigned read_calls=0;
bool file_bytes(uintptr_t address,uint8_t* out,size_t n){
 ++read_calls;if(read_fault)throw std::runtime_error("mapped read fault");
 const uintptr_t text=reinterpret_cast<uintptr_t>(game.data())+0x1000;
 if(scenario=="unreadable" || scenario=="mismatch" || scenario=="not-executable" || address<text || address-text>=0x100 || n>0x100-(address-text))return false;
 std::memcpy(out,disk.data()+0x400+(address-text),n);return true;
}
}
namespace kiln::compat {
'''+core_read+r'''
const OriginalCodeApi* selected=nullptr;
const OriginalCodeApi* original_code_table(){return selected;}
}
namespace kiln {
std::string_view sv(KilnStr s){return {s.ptr?s.ptr:"",s.len};}
'''+lookup+r'''
}
'''
main = r'''
int checks=0,failures=0;
void check(bool b,const char* label){++checks;if(!b){++failures;std::cerr<<"FAIL "<<label<<"\n";}}
template<class T>void put(std::vector<uint8_t>& b,size_t at,T value){std::memcpy(b.data()+at,&value,sizeof value);}
int fallback(const void* a,const void* b,size_t n){return std::memcmp(a,b,n);}
int main(int,char** argv){
 scenario=argv[1];exe=std::filesystem::path(argv[2])/"game-image.bin";
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
 if(scenario=="unreadable" || scenario=="core-snapshot")exe+=".missing";
 if(scenario=="mismatch")put<uint32_t>(game,0x48,43);
 if(scenario!="no-hook") {game[0x1040]=0xE9;put<int32_t>(game,0x1041,0x4000-(0x1040+5));}
 if(scenario=="suffix")game[0x1058]^=1;
 if(scenario=="not-entry")function_entry.BeginAddress=0x103F;
 if(scenario=="protect-failure")protect_fail=true;
 if(scenario=="restore-failure" || scenario=="restore-permanent")restore_fail=true;
 if(scenario=="cross-function")function_entry.EndAddress=0x1050;
 if(scenario=="unwind-outside")function_entry.EndAddress=0x3000;
 put<uint16_t>(mod,0,0x5A4D);put<uint32_t>(mod,0x3C,0x80);put<uint32_t>(mod,0x80,0x4550);put<uint16_t>(mod,0x84,0x8664);put<uint16_t>(mod,0x94,0xF0);put<uint16_t>(mod,0x98,0x20B);
 put<uint32_t>(mod,0x98+56,mod.size());put<uint32_t>(mod,0x98+108,16);put<uint32_t>(mod,0x98+120,0x200);put<uint32_t>(mod,0x98+124,40);
 put<uint32_t>(mod,0x200,0x300);put<uint32_t>(mod,0x20C,0x400);put<uint32_t>(mod,0x210,0x500);std::memcpy(mod.data()+0x400,"VCRUNTIME140.dll",16);
 put<uint64_t>(mod,0x300,0x600);std::memcpy(mod.data()+0x602,scenario=="no-import"?"memcpy":"memcmp",7);put<uintptr_t>(mod,0x500,reinterpret_cast<uintptr_t>(&fallback));
 std::memcpy(mod.data()+0x700,signature.data(),signature.size());
 disk=file;
 const kiln::compat::OriginalCodeApi rules{{sizeof(kiln::compat::OriginalCodeApi),kiln::compat::kOriginalCodeVersion},nullptr,[](uint32_t rva){++used_calls;used_rva=rva;},
 &kiln::compat::original_read};
 kiln::compat::selected=&rules;
 const char service[]="kiln.internal.original-code";
 check(kiln::api_service({service,sizeof(service)-1},2)==&rules,"any KilnApi caller can reach the private table by name");
 check(!kiln::api_service({service,sizeof(service)-1},3),"private lookup refuses a newer version");
 const auto reads=kiln::sigdb::read_calls;
 uint8_t probe=0;
 check(!rules.read(base+0x1000,&probe,4131) && !rules.read(base+0x1000,nullptr,1) && !rules.read(base+0x1000,&probe,0) && kiln::sigdb::read_calls==reads,"core callback rejects oversized, null and empty reads before sigdb");
 kiln::sigdb::read_fault=true;check(!rules.read(base+0x1000,&probe,1),"a mapped-file fault cannot escape the core callback");kiln::sigdb::read_fault=false;
 const auto before_game=game;
 std::atomic<bool> stop{false},ready{false};std::atomic<unsigned> bad_answers{0},thread_calls{0};
 std::thread reader;
 if(scenario=="concurrent")reader=std::thread([&]{
  ready=true;
  while(!stop){
   auto p=std::atomic_ref<void*>(*reinterpret_cast<void**>(mod.data()+0x500)).load();
   const int answer=reinterpret_cast<int(*)(const void*,const void*,size_t)>(p)(game.data()+0x1040,mod.data()+0x700,32);
   if(answer && answer!=std::memcmp(game.data()+0x1040,mod.data()+0x700,32))++bad_answers;
   ++thread_calls;
  }
 });
 if(reader.joinable())while(!ready || !thread_calls)std::this_thread::yield();
 const size_t patched=kshroud::original::install(mod.data(),&rules);
 if(reader.joinable()){stop=true;reader.join();check(thread_calls && !bad_answers,"mod-owned threads may call memcmp during IAT publication");}
 const bool installed=scenario!="unreadable" && scenario!="mismatch" && scenario!="not-executable" && scenario!="no-import" && scenario!="protect-failure";
 check(patched==size_t(installed),"production adapter patches exactly when the original file and import are usable");
 using Compare=int(*)(const void*,const void*,size_t);Compare compare;std::memcpy(&compare,mod.data()+0x500,8);
 const bool answer=installed && scenario!="suffix" && scenario!="not-entry" && scenario!="no-hook" && scenario!="live-unreadable" && scenario!="signature-unreadable" && scenario!="relay-unreadable" && scenario!="missing-unwind" && scenario!="cross-function" && scenario!="unwind-outside";
 const int result=compare(game.data()+0x1040,mod.data()+0x700,signature.size());
 if(installed)check(publication_result==result,"the shim is ready at the instant its IAT pointer is published");
 check((result==0)==(answer || scenario=="no-hook"),"IAT call preserves or substitutes the comparison correctly");
 check(compare(mod.data()+0x700,game.data()+0x1040,signature.size())==-result,"both game-operand positions work");
 check(used_calls==unsigned(answer),"first original-code answer logs once");
 if(answer){check(used_rva==0x1040,"first-use log reports game rva");check(compare(game.data()+0x1041,mod.data()+0x701,2)==0,"partial jump operands compare through the real shim");}
 const uint8_t high[]={0xFF},low[]={0x01};check(compare(high,low,1)>0 && compare(low,high,1)<0,"outside-code comparisons retain unsigned memcmp ordering");
 check(compare(high,high,0)==0,"zero-byte comparison is unchanged");
 check(game==before_game,"neither install nor compare changes game code");
 check(!execute_removed,"IAT writes never remove execute permission from a running DLL page");
 if(installed && scenario!="restore-permanent")check(page_protection==PAGE_EXECUTE_READ,"IAT page protection is restored, including a transient restore failure");
 if(scenario=="restore-permanent")check(warnings==1 && page_protection==PAGE_EXECUTE_READWRITE,"a persistent restore failure keeps execution enabled and emits a warning");
 if(answer){
  mod[0x71F]^=1;
  check(compare(game.data()+0x1040,mod.data()+0x700,32)==std::memcmp(game.data()+0x1040,mod.data()+0x700,32),"a mismatched static signature keeps ordinary ordering");mod[0x71F]^=1;
  check(compare(game.data()+0x103F,mod.data()+0x6FF,33)==std::memcmp(game.data()+0x103F,mod.data()+0x6FF,33),"a range crossing into the entry keeps ordinary memcmp");
  std::memcpy(game.data()+0x1080,signature.data(),32);
  check(compare(game.data()+0x1040,game.data()+0x1080,32)==std::memcmp(game.data()+0x1040,game.data()+0x1080,32),"two game ranges keep ordinary memcmp");
  check(compare(game.data()+0x1040,signature.data(),32)==std::memcmp(game.data()+0x1040,signature.data(),32),"heap or stack code copies keep ordinary memcmp");
  check(compare(mod.data()+0x700,mod.data()+0x700,32)==0,"the mod's own buffers keep ordinary memcmp");
  uintptr_t own=reinterpret_cast<uintptr_t>(mod.data()+0x800);
  game[0x1040]=0xFF;game[0x1041]=0x25;std::memset(game.data()+0x1042,0,4);put<uintptr_t>(game,0x1046,own);
  check(compare(game.data()+0x1040,mod.data()+0x700,32)!=0,"a second check sees the mod's own direct hook");
  relay[0]=0xFF;relay[1]=0x25;std::memset(relay.data()+2,0,4);put<uintptr_t>(relay,6,own);put<uintptr_t>(game,0x1046,reinterpret_cast<uintptr_t>(relay.data()));
  check(compare(mod.data()+0x700,game.data()+0x1040,32)!=0,"a second check sees the mod's own MinHook relay");
 }
 std::cout<<scenario<<": "<<checks-failures<<"/"<<checks<<" checks passed\n";return failures?1:0;
}
'''
cpp=out/'test.cpp'
cpp.write_text(preamble+core_adapter+production+main)
subprocess.run(['c++','-std=c++20','-O1','-g','-Wall','-Wextra','-pthread','-fsanitize=address,undefined',
    '-I'+str(root/'native/plugins/shroudtopia/src'),'-I'+str(root/'native/plugins/compat'),str(cpp),'-o',str(out/'test')],check=True)
failed=False
for scenario in ['hook','no-hook','suffix','not-entry','unreadable','mismatch','not-executable','no-import','protect-failure','restore-failure','core-snapshot','concurrent','live-unreadable','signature-unreadable','relay-unreadable','missing-unwind','cross-function','unwind-outside','restore-permanent']:
    failed |= subprocess.run([str(out/'test'),scenario,str(out)]).returncode != 0
raise SystemExit(int(failed))
