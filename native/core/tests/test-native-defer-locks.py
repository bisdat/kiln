#!/usr/bin/env python3
"""Run production load_mod with concurrent state readers inside the foreign calls."""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[3]
source = Path(os.environ.get('KILN_NATIVE_SOURCE', root / 'native/core/src/native_mods.cpp')).read_text()
load = source[source.index('void load_mod('):source.index('void run_phase(')]
out = Path(os.environ['KILN_OUT']) / 'native-defer-locks'
out.mkdir(parents=True, exist_ok=True)
preamble = r'''
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "kiln_native.h"
namespace fs = std::filesystem;
using DWORD = unsigned;
using HMODULE = void*;
constexpr DWORD ERROR_DLL_INIT_FAILED = 1114;
enum class St { Pending, Loaded, Failed, Unsupported };
struct Dll { std::string file, status="pending"; int32_t error=0; double ms=-1; HMODULE module=nullptr; };
struct Mod {
 std::string id="doors",version="1.0.0",folder,init="kiln_native_init",cfg_dir,data_dir,reason;
 std::vector<std::string> needs,notes;std::vector<Dll> dlls={{"powrprof.dll"}};
 St state=St::Pending;bool loading=false,init_called=false,init_faulted=false;
 int load_index=-1,init_result=0;unsigned long init_fault=0;double init_ms=0;
 KilnOwner owner=0;KilnNativeContext ctx{};
};
std::recursive_mutex g_mutex;
std::set<std::wstring> g_loaded_paths;
int g_next_index=0,loads=0,inits=0,checks=0,failures=0;
bool fail_load=false,fail_init=false,unsupported=false;
const char* kPhases[]={"dllmain","early","boot","world"};
void check(bool b,const char* s){++checks;if(!b){++failures;std::cerr<<"FAIL "<<s<<"\n";}}
void state_reader(const char* label){
 bool available=false;std::thread reader([&]{available=g_mutex.try_lock();if(available)g_mutex.unlock();});reader.join();
 check(available,label);
}
void fail(Mod& m,St s,std::string reason){m.state=s;m.reason=std::move(reason);}
const Mod* any_by_id(const std::string&){return nullptr;}
struct Paths {fs::path config;} test_paths;
const Paths& paths(){return test_paths;}
std::wstring to_wide(const std::string& s){return {s.begin(),s.end()};}
std::string u8(const fs::path& p){return p.string();}
std::wstring final_path(const fs::path& p){return p.wstring();}
namespace profile {struct State {std::string data_dir;} test_state;const State& state(){return test_state;}}
namespace reg {KilnOwner owner_create(const std::string&){return 1;}void owner_revoke(KilnOwner){}}
double now_ms(){return 100;}
const KilnApi* api(){return nullptr;}
struct Image {uint32_t side=KILN_SIDE_CLIENT;};const Image& image(){static Image i;return i;}
bool hooks_enabled(){return true;}
struct Boot {uint32_t play_once=0;};const Boot& boot_info(){static Boot b;return b;}
KilnStr kstr(const std::string& s){return {s.data(),s.size()};}
#define KLOG_I(...) ((void)0)
#define KLOG_W(...) ((void)0)
#define KLOG_E(...) ((void)0)
HMODULE guarded_load(const char*,DWORD* error){
 ++loads;state_reader("state readers can run while LoadLibrary is inside DllMain");
 *error=fail_load?1114:0;return fail_load?nullptr:reinterpret_cast<HMODULE>(1);
}
int native_init(const KilnNativeContext*){return 0;}
auto GetProcAddress(HMODULE,const char*){return &native_init;}
bool guarded_init(KilnNativeInitFn,const KilnNativeContext*,int* result,unsigned long* fault){
 ++inits;state_reader("state readers can run during a foreign native init");
 *result=unsupported?KILN_NATIVE_INIT_UNSUPPORTED:KILN_NATIVE_INIT_OK;*fault=fail_init?1:0;return !fail_init;
}
'''
main = r'''
int main(int,char** argv){
 fs::path game=argv[1];fs::create_directories(game);std::ofstream(game/"powrprof.dll")<<"stub";
 test_paths.config=game/"config";profile::test_state.data_dir=game.string();
 for(int mode=0;mode<4;++mode){
  Mod m;m.folder=game.string();fail_load=mode==1;fail_init=mode==2;unsupported=mode==3;
  load_mod(m,1);
  check(m.state==(mode==0?St::Loaded:mode==3?St::Unsupported:St::Failed),"normal load, failure, fault and unsupported outcomes are preserved");
  check(!m.loading,"load ownership is cleared on every exit path");
  if(mode==0){check(m.ctx.id.ptr==m.id.data() && m.ctx.directory.ptr==m.folder.data(),"init context strings retain their stable mod storage");
   int before=loads;load_mod(m,1);check(loads==before,"a completed native cannot load twice");}
 }
 Mod busy;busy.folder=game.string();busy.loading=true;int before=loads;load_mod(busy,1);
 check(loads==before,"an in-progress native cannot load twice");
 std::cout<<checks-failures<<"/"<<checks<<" checks passed\n";return failures?1:0;
}
'''
(out / 'test.cpp').write_text(preamble + load + main)
subprocess.run(['c++', '-std=c++20', '-O0', '-g0', '-pthread', '-I'+str(root/'native/core/abi'),
                str(out/'test.cpp'), '-o', str(out/'test')], check=True)
subprocess.run([str(out/'test'), str(out/'game')], check=True)
