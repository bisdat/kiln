#!/usr/bin/env python3
"""Host checks of production compat parsing and native deferral. No Wine or game."""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[3]
out = Path(os.environ['KILN_OUT']) / 'native-defer'
out.mkdir(parents=True, exist_ok=True)
(out / 'windows.h').write_text(r'''
#pragma once
#include <string>
#include <codecvt>
#include <locale>
#include <cwctype>
#include <algorithm>
#include <map>
#include <cstdint>
using DWORD=uint32_t;using LONG=int32_t;using LONG64=int64_t;using HANDLE=void*;
#define FALSE 0
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(-1)
constexpr unsigned PAGE_READWRITE=4,FILE_MAP_ALL_ACCESS=0xf001f;
struct Page {alignas(8) unsigned char bytes[16]{};};
inline std::map<std::wstring,Page> pages;
inline uint64_t fake_tick=100,fake_created=1234;inline DWORD fake_pid=42,last_error=0;
inline bool times_fail=false;
struct FILETIME {DWORD dwLowDateTime=0,dwHighDateTime=0;};
constexpr DWORD ERROR_ALREADY_EXISTS=183;
inline HANDLE GetCurrentProcess(){return reinterpret_cast<HANDLE>(1);}
inline int GetProcessTimes(HANDLE,FILETIME* created,FILETIME*,FILETIME*,FILETIME*){
 created->dwLowDateTime=DWORD(fake_created);created->dwHighDateTime=DWORD(fake_created>>32);return !times_fail;
}
inline DWORD GetLastError(){return last_error;}
inline DWORD GetCurrentProcessId(){return fake_pid;}
inline uint64_t GetTickCount64(){return fake_tick;}
inline HANDLE CreateFileMappingW(HANDLE,void*,unsigned,unsigned,unsigned,const wchar_t* name){last_error=pages.count(name)?ERROR_ALREADY_EXISTS:0;return &pages[name];}
inline HANDLE OpenFileMappingW(unsigned,int,const wchar_t* name){auto it=pages.find(name);return it==pages.end()?nullptr:&it->second;}
inline void* MapViewOfFile(HANDLE h,unsigned,unsigned,unsigned,size_t){return static_cast<Page*>(h)->bytes;}
inline void UnmapViewOfFile(void*){}
inline void CloseHandle(HANDLE){}
inline LONG InterlockedExchange(LONG* p,LONG v){auto old=*p;*p=v;return old;}
inline LONG64 InterlockedExchange64(LONG64* p,LONG64 v){auto old=*p;*p=v;return old;}
inline LONG InterlockedCompareExchange(LONG* p,LONG v,LONG old){auto x=*p;if(x==old)*p=v;return x;}
inline LONG64 InterlockedCompareExchange64(LONG64* p,LONG64 v,LONG64 old){auto x=*p;if(x==old)*p=v;return x;}
constexpr unsigned CP_UTF8=65001;
inline int MultiByteToWideChar(unsigned,unsigned,const char* s,int n,wchar_t* p,int) {
 auto w=std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(s,s+n);
 if(p) std::copy(w.begin(),w.end(),p); return int(w.size());
}
inline DWORD CharLowerBuffW(wchar_t* p,DWORD n) { for(DWORD i=0;i<n;++i)p[i]=std::towlower(p[i]);return n; }
''')
source = r'''
#include "compat_rules.h"
#include "first_pass.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <clocale>
#include <nlohmann/json.hpp>
using namespace kiln::compat;
int checks=0, failures=0;
void check(bool b,const char* label){++checks;if(!b){++failures;std::cerr<<"FAIL "<<label<<"\n";}}
int main(int, char** argv){
 std::setlocale(LC_CTYPE,"C.UTF-8");

 using namespace kiln::first_pass;
 Channel absent;check(!absent.open(false),"host open cannot create a missing channel");
 Channel core,host;check(core.open(true) && host.open(false),"core and host open same process channel");
 check(core.state()==State::Expected,"channel starts with expected host");
 host.done();check(core.state()==State::Expected,"completion cannot precede host startup");
 host.begin();check(core.state()==State::Running && core.started()==100,"host publishes start tick before running state");
 core.absent();check(core.state()==State::Running,"plugins_ready cannot release a running host");
 fake_tick=30100;check(core.decision(30000)==Decision::Timeout,"actual channel enforces host-start deadline");
 host.done();check(core.decision(30000)==Decision::Complete,"completion is visible to core");
 host.bypass();core.absent();check(core.state()==State::Done,"stop/fault fallback preserves completed pass");
 host.stop();check(core.decision(30000)==Decision::Cancel,"shutdown cancels pending startup");
 fake_pid=43;Channel other;check(other.open(true) && other.state()==State::Expected,"process ids isolate channels");
 other.absent();check(other.decision(30000)==Decision::Bypass,"absent host does not wait");
 fake_pid=42;++fake_created;Channel reused,reused_host;
 check(reused.open(true) && reused_host.open(false) && reused.state()==State::Expected,"reused pid cannot inherit an earlier process's stopped or completed pass");
 reused_host.begin();check(reused.state()==State::Running && core.state()==State::Stopped,"reused pid has a distinct channel");
 Channel collision;check(!collision.open(true),"core refuses an already-existing channel instead of trusting its contents");
 ++fake_pid;times_fail=true;Channel no_identity;check(!no_identity.open(true),"missing process identity fails open without creating a wait");times_fail=false;
 std::ifstream shipped_file(argv[2]);std::stringstream shipped_text;shipped_text<<shipped_file.rdbuf();
 auto shipped=parse_rules(shipped_text.str(),"shipped");check(shipped.ok && shipped.dropped==0 && shipped.rules.size()==2,"all shipped rules parse");
 Facts package_start;package_start.mods={"shroudtopia:item-skill-text"};Facts package_own;
 package_own.mods={"native:auto-doors-new-version"};package_own.modules={"powrprof.dll","AutoDoors/bin/ad_core.dll"};
 // 0.2.0-dev.29.5 (inbox 33): Auto Doors started late does nothing (it must start before the game registers its programs),
 // so no shipped rule defers it any more; the defer action stays for rules a player or a later Kiln writes.
 check(!native_defer_rule(shipped.rules,package_start,package_own,true),"no shipped rule defers Auto Doors");
 package_own.modules={"powrprof.dll"};
 check(!native_defer_rule(shipped.rules,package_start,package_own,true),"shipped rule never matches an unrelated powrprof provider");
 Facts original_start;
 const auto* original=original_code_rule(shipped.rules,original_start,"shroudtopia:item-skill-text");
 check(original && original->id=="shroudtopia-item-skill-text-original-code","shipped rule selects only item-skill-text");
 check(!original_code_rule(shipped.rules,original_start,"shroudtopia:other"),"another Shroudtopia mod is not selected");
 auto conditional=shipped.rules;
 for(auto& r:conditional)if(r.action=="original-code")r.when={{Matcher::Kind::Engine,"test-host"}};
 check(!original_code_rule(conditional,original_start,"shroudtopia:item-skill-text"),"original-code respects start conditions");
 original_start.engines={"test-host"};
 check(original_code_rule(conditional,original_start,"shroudtopia:item-skill-text"),"matching condition enables the named rule");
 const std::string rule=R"({"format":1,"rules":[{"id":"doors","action":"defer","when":[{"mod":"shroudtopia:item-skill-text"}],"subject":{"mod":"native:auto-doors-1.1.0-298-1.1.0-2026-11-04t19-12z-axkrtkhdp"},"target":{"mod":"shroudtopia:item-skill-text"},"max_wait_ms":30000}]})";
 auto p=parse_rules(rule,"test");
 check(p.ok && p.rules.size()==1 && !p.dropped,"accept bounded first-pass deferral rule");

 if (!p.rules.empty()) {
  Facts start;start.mods={"shroudtopia:item-skill-text"};Facts own;own.mods={"native:auto-doors-1.1.0-298-1.1.0-2026-11-04t19-12z-axkrtkhdp"};
  check(native_defer_rule(p.rules,start,own,true)!=nullptr,"reported Auto Doors defers");
  check(!native_defer_rule(p.rules,start,own,false),"absent or external host does not defer");
  auto disabled=start;disabled.mods.clear();check(!native_defer_rule(p.rules,disabled,own,true),"disabled target does not defer");
  auto unrelated=own;unrelated.mods={"native:other"};check(!native_defer_rule(p.rules,start,unrelated,true),"other native does not defer");
  auto module_rule=p.rules;module_rule[0].subject={Matcher::Kind::Module,"powrprof.dll"};own.modules={"POWRPROF.DLL"};
  check(native_defer_rule(module_rule,start,own,true)!=nullptr,"module subject uses existing case-insensitive matching");
  module_rule[0].target.value="shroudtopia:other";check(!native_defer_rule(module_rule,start,own,true),"target must be on even if when matches");
 }
 auto doc=nlohmann::json::parse(rule);
 for(auto invalid : {nlohmann::json(0),nlohmann::json(-1),nlohmann::json(30001),nlohmann::json(1.5),nlohmann::json("30000"),nlohmann::json(nullptr)}){
  doc["rules"][0]["max_wait_ms"]=invalid; check(parse_rules(doc.dump(),"test").dropped==1,"reject invalid timeout");
 }
 doc=nlohmann::json::parse(rule); doc["rules"][0].erase("max_wait_ms"); check(parse_rules(doc.dump(),"test").dropped==1,"timeout required");
 doc=nlohmann::json::parse(rule); doc["rules"][0]["target"]={{"engine","shroudtopia"}}; check(parse_rules(doc.dump(),"test").dropped==1,"target must name a Shroudtopia mod");
 doc["rules"][0]["target"]={{"mod","emf:item-skill-text"}};check(parse_rules(doc.dump(),"test").dropped==1,"other hosts cannot be waited on");
 doc=nlohmann::json::parse(rule);doc["rules"][0]["action"]="warn";doc["rules"][0].erase("target");check(parse_rules(doc.dump(),"test").dropped==1,"timeout belongs only to defer");
 // Hold the existing cross-language parser/glob fixtures to their contract, too.
 std::ifstream f(argv[1]); nlohmann::json fixtures;f>>fixtures;
 for(const auto& c:fixtures["parse"]){
  const auto text=c["input"].is_string()?c["input"].get<std::string>():c["input"].dump();auto r=parse_rules(text,"test");
  std::vector<std::string> ids;for(const auto& v:r.rules)ids.push_back(v.id);
  check(r.ok==c["ok"].get<bool>() && ids==c["rules"].get<std::vector<std::string>>() && r.dropped==c["dropped"].get<int>(),c["name"].get<std::string>().c_str());
 }
 for(const auto& c:fixtures["glob"])check(glob_match(c["pattern"].get<std::string>(),c["name"].get<std::string>())==c["match"].get<bool>(),"shared glob fixture");
 std::cout<<checks-failures<<"/"<<checks<<" checks passed\n";return failures?1:0;
}
'''
(out/'test.cpp').write_text(source)
subprocess.run(['c++','-std=c++20','-O0','-g0','-I'+str(out),'-I'+str(root/'native/core/src'),'-I'+str(root/'native/plugins/compat'),'-I'+str(root/'native/third_party'),str(out/'test.cpp'),str(root/'native/core/src/compat_rules.cpp'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test'),str(root/'schema/fixtures/compat-rules/cases.json'),str(root/'native/core/compat-rules.json')],check=True)
