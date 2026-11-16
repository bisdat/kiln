#!/usr/bin/env python3
"""Execute the production phase runner and wait worker with fake clock and loader calls."""
import os
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[3]
plugin=Path(os.environ.get('KILN_NATIVE_SOURCE',root/'native/core/src/native_mods.cpp')).read_text()
phase=plugin[plugin.index('void run_phase('):plugin.index('const char* const kChainSources[]')]
worker=plugin[plugin.index('// native-defer:begin'):plugin.index('// native-defer:end')] if '// native-defer:begin' in plugin else 'DWORD deferred_main(void*) { return 0; }'
start=plugin[plugin.index('void start_deferred() {'):plugin.index('\nvoid run_late()',plugin.index('void start_deferred() {'))] if 'void start_deferred() {' in plugin else 'void start_deferred() {} void plugins_ready() {}'
early=plugin[plugin.index('void run_early()'):plugin.index('void start_deferred() {')] if 'void start_deferred() {' in plugin else 'void run_early(){run_phase(1);}'
boot=(root/'native/core/src/boot.cpp').read_text()
boot=boot[boot.index('    if (hooks) load_plugins();'):boot.index('    if (hooks) boot_svc::bound();')]
out=Path(os.environ['KILN_OUT'])/'native-scheduler';out.mkdir(parents=True,exist_ok=True)
pre=r'''
#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <functional>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cwctype>
#include "compat_rules.h"
#include "first_pass_policy.h"
namespace first_pass=kiln::first_pass;
#define KLOG_I(...) log_line(__VA_ARGS__)
#define KLOG_W(...) log_line(__VA_ARGS__)
#define WINAPI
using DWORD=unsigned;using HANDLE=void*;
constexpr unsigned KILN_NATIVE_PHASE_DLLMAIN=0,KILN_NATIVE_PHASE_EARLY=1,KILN_NATIVE_PHASE_WORLD=3,KILN_PROFILE_KIND_PLUGIN=2;
enum class St {Pending,Loaded,Failed,Skipped};
struct Dll {std::string file;};
struct Mod {std::string id;unsigned phase=1;std::vector<std::string> needs,after,before,notes;std::vector<Dll> dlls;St state=St::Pending;bool phase_reached=false;std::optional<kiln::compat::Rule> wait_rule;bool fail=false;};
std::recursive_mutex g_mutex;
std::vector<Mod*> g_order[4];std::vector<Mod> g_mods;
bool g_defer_worker=false,g_defer_ready=false,thread_failure=false,g_verified=true,external=false,channel_failure=false;
namespace fs=std::filesystem;
struct Config {bool plugins=true,policy_any=false;} cfg;
const Config& config(){return cfg;}
struct Paths {fs::path plugins,game;} test_paths;
const Paths& paths(){return test_paths;}
std::wstring to_wide(const std::string& s){return {s.begin(),s.end()};}
std::string u8(const fs::path& p){return p.string();}
std::wstring lower(std::wstring s){for(auto& c:s)c=std::towlower(c);return s;}
namespace profile {
struct Entry {bool enabled=true;std::string path;};
struct State {std::vector<Entry> mods[4];} test_state;
const State& state(){return test_state;}
}
namespace kshroud {std::string own_loader_in_process(const fs::path&){return external?"external loader":"";}}
namespace compat=kiln::compat;
namespace kiln::compat {
Facts test_facts;std::vector<Rule> test_rules;
std::vector<Rule> native_wait_rules(Facts* f){*f=test_facts;return test_rules;}
const Rule* native_defer_rule(const std::vector<Rule>& rules,const Facts& f,const Facts& own,bool host){
 if(!host || f.mods.empty() || own.mods!=std::vector<std::string>{"native:doors"} || rules.empty())return nullptr;
 return &rules[0];
}
}
const char* kPhases[]={"dllmain","early","boot","world"};const char* g_last_phase=nullptr;
uint64_t clock_ms=100;std::vector<std::string> loads,logs;unsigned writes=0,sleeps=0;
std::function<void()> on_sleep;DWORD (*thread_fn)(void*)=nullptr;
void log_line(const char* f,...) {logs.push_back(f);}
uint64_t GetTickCount64(){return clock_ms;}
void Sleep(unsigned n){++sleeps;clock_ms+=n;if(on_sleep)on_sleep();}
HANDLE CreateThread(void*,size_t,DWORD (*fn)(void*),void*,unsigned,void*){thread_fn=fn;return thread_failure?nullptr:reinterpret_cast<HANDLE>(1);}
void CloseHandle(HANDLE){}
struct Channel {
 first_pass::State status=first_pass::State::Expected;uint64_t start=100;
 bool open(bool){return !channel_failure;}
 auto decision(unsigned n)const{return first_pass::decide(status,start,clock_ms,n);}
 uint64_t started()const{return start;}
 void absent(){if(status==first_pass::State::Expected)status=first_pass::State::Bypass;}
 void bypass(){status=first_pass::State::Bypass;}
}g_first_pass;
void write_state(){++writes;}
void fail(Mod& m,St state,const std::string&){m.state=state;}
void load_mod(Mod& m,unsigned){loads.push_back(m.id);m.state=m.fail?St::Failed:St::Loaded;
 for(const auto& need:m.needs)for(const auto& d:g_mods)if(d.id==need && d.state!=St::Loaded)m.state=St::Failed;}
void start_deferred();
void plugins_ready();
void load_plugins(){g_first_pass.status=first_pass::State::Running;}
namespace progress_state {void step(const char*){}}
namespace native_mods {void plugins_ready(){::plugins_ready();}}
'''
main=r'''
int checks=0,failures=0;
void check(bool b,const char* s){++checks;if(!b){++failures;std::cerr<<"FAIL "<<s<<"\n";}}
void setup(){
 g_mods.clear();g_mods.resize(3);for(auto& o:g_order)o.clear();loads.clear();logs.clear();g_defer_worker=g_defer_ready=false;thread_failure=external=channel_failure=false;g_verified=true;cfg={};compat::test_facts.mods={"shroudtopia:item-skill-text"};compat::test_rules={compat::Rule{}};compat::test_rules[0].target.value="shroudtopia:item-skill-text";compat::test_rules[0].max_wait_ms=100;profile::test_state={};std::ofstream(test_paths.plugins/"kiln-shroudtopia.dll")<<"stub";thread_fn=nullptr;sleeps=writes=0;clock_ms=100;on_sleep={};
 g_first_pass.status=first_pass::State::Expected;g_first_pass.start=100;
 g_mods[0].id="doors";g_mods[0].wait_rule=kiln::compat::Rule{};g_mods[0].wait_rule->max_wait_ms=100;
 g_mods[1].id="ordinary";g_mods[2].id="boot";g_mods[2].phase=2;
 g_order[1]={&g_mods[0],&g_mods[1]};g_order[2]={&g_mods[2]};
}
int main(int,char** argv){
 test_paths.game=argv[1];test_paths.plugins=test_paths.game/"plugins";fs::create_directories(test_paths.plugins);
 setup();run_phase(1);check(loads==std::vector<std::string>{"ordinary"},"early phase holds doors and starts unrelated native");
 check(!g_mods[2].phase_reached,"boot native not started early");
 g_first_pass.status=first_pass::State::Running;start_deferred();check(thread_fn && g_defer_worker,"wait has its own worker");
 check(loads==std::vector<std::string>{"ordinary"},"starting worker does not synchronously start doors");
 on_sleep=[] {check(g_mods[0].state==St::Pending,"held during first pass");g_first_pass.status=first_pass::State::Done;};
 deferred_main(nullptr);check(loads==std::vector<std::string>({"ordinary","doors"}) && sleeps==1,"start exactly once after first pass");
 check(!g_defer_worker && writes>=2,"completion writes state and retires worker");start_deferred();check(loads.size()==2,"no duplicate start");
 run_phase(2);check(loads.back()=="boot","later phase still runs normally");
 setup();run_phase(1);g_first_pass.status=first_pass::State::Running;start_deferred();deferred_main(nullptr);
 check(g_mods[0].state==St::Loaded && clock_ms==200 && sleeps==4,"timeout measured from host start and bounded");
 check(logs.size()>=3,"timeout emits warning and startup log");
 setup();run_phase(1);clock_ms=1000;g_first_pass.status=first_pass::State::Running;start_deferred();deferred_main(nullptr);
 check(sleeps==0 && g_mods[0].state==St::Loaded,"time spent in other on_ready callbacks counts toward timeout");
 for(auto state:{first_pass::State::Expected,first_pass::State::Bypass,first_pass::State::Done}){
  setup();run_phase(1);g_first_pass.status=state;plugins_ready();deferred_main(nullptr);
  check(sleeps==0 && g_mods[0].state==St::Loaded,"absent, stepped aside, or completed host starts immediately");
 }
 setup();run_phase(1);start_deferred();check(g_first_pass.status==first_pass::State::Expected,"worker waits for host startup without declaring it absent");
 on_sleep=[] {if(clock_ms>=125)g_first_pass.status=first_pass::State::Running;};deferred_main(nullptr);
 check(g_mods[0].state==St::Loaded && clock_ms==200,"worker enforces timeout while plugin callbacks have not returned");
 setup();run_phase(1);g_mods[0].fail=true;g_first_pass.status=first_pass::State::Done;start_deferred();deferred_main(nullptr);
 check(g_mods[0].state==St::Failed && !g_defer_worker,"normal native failure retained");
 setup();run_phase(1);thread_failure=true;g_first_pass.status=first_pass::State::Running;start_deferred();
 check(sleeps==0 && g_mods[0].state==St::Loaded,"worker creation failure fails open without a wait");
 setup();g_mods[2].wait_rule=g_mods[0].wait_rule;run_phase(1);g_first_pass.status=first_pass::State::Running;plugins_ready();run_phase(2);
 on_sleep=[] {g_first_pass.status=first_pass::State::Done;};deferred_main(nullptr);
 check(loads==std::vector<std::string>({"ordinary","doors","boot"}),"deferred boot native uses its own phase and follows early natives");
 setup();g_order[1].clear();g_order[3]={&g_mods[0]};g_first_pass.status=first_pass::State::Running;plugins_ready();run_phase(3);
 on_sleep=[] {g_first_pass.status=first_pass::State::Done;};deferred_main(nullptr);
 check(g_mods[0].state==St::Loaded,"world native can start its own deferred worker");

 for(int mode=0;mode<6;++mode){
  setup();for(auto& m:g_mods)m.wait_rule.reset();
  if(mode==0)fs::remove(test_paths.plugins/"kiln-shroudtopia.dll");
  if(mode==1)external=true;
  if(mode==2)cfg.plugins=false;
  if(mode==3)g_verified=false;
  if(mode==4)compat::test_facts.mods.clear();
  if(mode==5)channel_failure=true;
  run_early();check(!g_mods[0].wait_rule && g_mods[0].state==St::Loaded,"preflight bypass: absent/external/off/safe-idle/disabled/channel failure");
 }
 setup();for(auto& m:g_mods)m.wait_rule.reset();run_early();
 check(g_mods[0].wait_rule && g_mods[0].state==St::Pending && g_mods[1].state==St::Loaded && thread_fn,"real early runner plans wait and launches worker before on_ready");
 for(int edge=0;edge<3;++edge){
  setup();for(auto& m:g_mods)m.wait_rule.reset();
  if(edge==0)g_mods[1].needs={"doors"};if(edge==1)g_mods[1].after={"doors"};if(edge==2)g_mods[0].before={"ordinary"};
  g_mods[2].needs={"ordinary"};run_early();
  check(g_mods[1].wait_rule && g_mods[2].wait_rule && loads.empty(),"wait preserves requires/after/before and transitive later-phase dependencies");
  g_first_pass.status=first_pass::State::Done;plugins_ready();run_phase(2);deferred_main(nullptr);
  check(loads==std::vector<std::string>({"doors","ordinary","boot"}) && g_mods[2].state==St::Loaded,"completed pass retains declared order across phases");
 }
 setup();run_phase(1);g_first_pass.status=first_pass::State::Stopped;start_deferred();
 on_sleep=[] {g_first_pass.status=first_pass::State::Done;};deferred_main(nullptr);
 check(g_mods[0].state==St::Skipped && loads==std::vector<std::string>{"ordinary"},"host shutdown cancels deferred startup");
 setup();run_phase(1);simulate_boot(true);check(g_first_pass.status==first_pass::State::Running,"boot only finalizes the wait after plugin callbacks");
 setup();run_phase(1);simulate_boot(false);deferred_main(nullptr);check(sleeps==0 && g_mods[0].state==St::Loaded,"safe-idle boot releases an expected host");
 std::cout<<checks-failures<<"/"<<checks<<" checks passed\n";return failures?1:0;
}
'''
(out/'test.cpp').write_text(pre+phase+worker+early+start+"\nvoid simulate_boot(bool hooks){\n"+boot+"}\n"+main)
subprocess.run(['c++','-std=c++20','-O0','-g0','-pthread','-I'+str(root/'native/core/src'),'-I'+str(root/'native/plugins/compat'),str(out/'test.cpp'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test'),str(out/'game')],check=True)
