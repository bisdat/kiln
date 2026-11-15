#!/usr/bin/env python3
"""Linux host regression: compile the production config and scheduling code, no Wine/game.
Only platform I/O and foreign-call guards are substituted; config mutations and lifecycle are real.
Run through the review slot, with KILN_OUT set to this task's build folder.
"""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[4]
context = (root / 'native/plugins/shroudtopia/src/context.cpp').read_text()
plugin = (root / 'native/plugins/shroudtopia/src/plugin.cpp').read_text()
config = context[context.index('namespace config {'):context.index('// ---------------------------------------------------------------------------------------------- memory')]
loaded = plugin[plugin.index('struct Loaded {'):plugin.index('ModContext* g_ctx')]
scheduling = plugin[plugin.index('void drive('):plugin.index('int json_int(')]
# Exercise the exact registration hook, including the origin gate and metadata name.
hook = 'config::ensure_profile_entry(m->name, m->origin == "profile");'
registration = hook if hook in plugin[plugin.index('m->mod = inst;'):plugin.index('// Shroudtopia\'s UpdateMods.')] else ''
preamble = r'''
#include <filesystem>
#include <fstream>
#include <mutex>
#include <atomic>
#include <set>
#include <vector>
#include <memory>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
namespace fs = std::filesystem;
constexpr int KILN_LOG_INFO=1, KILN_LOG_WARN=2, KILN_LOG_ERROR=3;
struct Host { template<class... T> void logf(T...) {} } H;
std::atomic<bool> g_log_enabled{true};
namespace kcompat {
bool fail_write=false;
bool exists(const fs::path& p) { return fs::exists(p); }
std::string path_utf8(const fs::path& p) { return p.string(); }
bool read_file(const fs::path& p, std::string* s) { std::ifstream f(p); if (!f) return false; *s={std::istreambuf_iterator<char>(f),{}}; return true; }
bool write_file_atomic(const fs::path& p, const std::string& s) {
 if (fail_write) return false;
 fs::create_directories(p.parent_path()); auto temp=p; temp += ".tmp";
 std::ofstream f(temp); f<<s; f.close(); if (!f) return false; fs::rename(temp,p); return true;
}
}
using DWORD=unsigned;
template<class F> bool guarded(F f, DWORD*) { f(); return true; }
void set_current_mod(const std::string&) {}
std::string fault_text(DWORD) { return "fault"; }
void loader_log(int, const std::string&) {}
struct ModContext { std::set<void*> loadedMods; } ctx, *g_ctx=&ctx;
struct Mod {
 bool loaded=false, active=false;
 void Load(ModContext*) {} void Activate(ModContext*) {active=true;}
 void Update(ModContext*) {} void Deactivate(ModContext*) {active=false;}
};
'''
main = r'''
std::mutex g_mu;
std::vector<std::unique_ptr<Loaded>> g_mods;
bool g_server=false;
'''+scheduling+r'''
int failures=0, checks=0;
void check(bool ok, const char* label) { ++checks; if (!ok) { ++failures; std::cerr<<"FAIL "<<label<<"\n"; } }
int main(int argc, char** argv) {
 fs::path file=fs::path(argv[1])/"shroudtopia.json";
 Mod mod;
 auto run=[&](json original, std::string origin, json manifest) {
  kcompat::write_file_atomic(file,original.dump());
  config::init(file,file.parent_path()/"absent.json");
  config::apply_manifest_defaults("library-id",manifest);
  auto m=std::make_unique<Loaded>(); mod=Mod{};
  m->mod=&mod; m->name="Reported Name.With Dot"; m->id="library-id"; m->origin=origin; m->client=true;
  REGISTRATION
  g_mods.clear(); g_mods.push_back(std::move(m)); update_mods();
  std::string text; kcompat::read_file(file,&text);
  return std::pair{g_mods[0]->active,json::parse(text)};
 };
 json base={{"active",true},{"unknown",{{"keep",json::array({1,"text",false})}}},{"mods",{{"Other",{{"active",false},{"custom",42}}}}}};
 auto [active,doc]=run(base,"profile",nullptr);
 check(active,"DLL-only profile mod activates first start");
 check(doc["mods"]["Reported Name.With Dot"]==json({{"active",true}}),"persists exact reported name, not id/stem/dotted path");
 doc["mods"].erase("Reported Name.With Dot"); check(doc==base,"all other content survives");
 auto off=base; off["mods"]["Reported Name.With Dot"]={{"active",false},{"custom",9}};
 auto [disabled,unchanged]=run(off,"profile",nullptr);
 check(!disabled && unchanged==off,"existing false entry untouched");
 auto [dropin,unlisted]=run(base,"folder",nullptr);
 check(!dropin && unlisted==base,"not profile-enabled: no entry or activation");
 auto [folder,folderDoc]=run(base,"profile",json::object());
 check(folder && folderDoc["mods"].contains("Reported Name.With Dot"),"profile folder without defaults activates");
 auto existing=base; existing["mods"]["Reported Name.With Dot"]={{"custom",7}};
 auto [missingFlag,kept]=run(existing,"profile",nullptr);
 check(!missingFlag && kept==existing,"existing entry without active untouched");
 auto defaults=json{{"shroudtopia",{{"default",{{"active",false},{"option",4}}}}}};
 auto compatible=base; compatible["mods"]["Reported Name.With Dot"]={{"active",true}};
 auto [oldActive,oldDoc]=run(compatible,"profile",defaults);
 check(oldActive && oldDoc["mods"]["library-id"]==defaults["shroudtopia"]["default"],"existing activation and folder defaults preserved");
 // Live file reload still deactivates the mod.
 oldDoc["mods"]["Reported Name.With Dot"]["active"]=false;
 kcompat::write_file_atomic(file,oldDoc.dump()); config::reload_if_changed(); update_mods();
 check(!g_mods[0]->active,"live reload deactivates");
 // Creation must not overwrite external edits or invalid settings observed after init.
 auto m=std::make_unique<Loaded>(); m->name="New Name"; m->origin="profile";
 auto ensure=[&] { REGISTRATION }; // same hook as registration
 config::init(file,file.parent_path()/"absent.json");
 auto fresh=oldDoc; fresh["external"]="new edit";
 kcompat::write_file_atomic(file,fresh.dump()); ensure();
 std::string text; kcompat::read_file(file,&text); auto merged=json::parse(text);
 merged["mods"].erase("New Name"); check(merged==fresh,"merges latest disk values");
 std::string first; kcompat::read_file(file,&first); ensure();
 kcompat::read_file(file,&text); check(text==first,"repeat registration makes no write");
 m->name="Another Name";
 for (const std::string broken : {"{invalid", "{\"mods\":false}"}) {
  kcompat::write_file_atomic(file,broken); ensure();
  kcompat::read_file(file,&text); check(text==broken,"invalid settings not overwritten");
 }
 kcompat::write_file_atomic(file,fresh.dump()); kcompat::fail_write=true; ensure(); kcompat::fail_write=false;
 kcompat::read_file(file,&text);
 check(json::parse(text)==fresh && config::mod_get("Another Name","active").is_null(),"failed save does not activate in memory");
 std::cout<<checks-failures<<"/"<<checks<<" checks passed\n"; return failures?1:0;
}
'''
out = Path(os.environ['KILN_OUT']) / 'shroudtopia-host'
out.mkdir(parents=True, exist_ok=True)
source = out / 'test.cpp'
source.write_text(preamble + config + loaded + main.replace('REGISTRATION',registration))
subprocess.run(['c++','-std=c++20','-O0','-g0','-pthread','-I'+str(root/'native/third_party'),str(source),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test'),str(out/'settings')],check=True)
