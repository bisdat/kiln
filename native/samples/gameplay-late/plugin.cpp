// SPDX-License-Identifier: MIT
// Passive lab plugin. No Gameplay request or subscription at load: only explicit control-file commands on Pre.
#include "kiln_api.h"
#include "services/kiln_gameplay.h"
#include <array>
#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
namespace late {
const KilnApi* api=nullptr; const KilnGameplayApi* gameplay=nullptr; KilnOwner owner=0;
std::filesystem::path control; bool enabled=false;
uint64_t pres=0,joined=0,left=0; std::vector<KilnHandle> subscriptions; uint32_t subscribed=0;
const char* names[]={"build","craft","terraform","spawn","entities"};
struct Progress { bool active=false,waiting=false,done=false; uint64_t began=0,early=0,events=0; } progress[5];
uint32_t zone_id=0, zone_deny=0; int zone_group=-1; bool zone_waiting=false;
KilnGameplayZone zone{}; uint64_t no_owners=0;
bool replacement_pending=false; uint8_t replacement_to[16]{};
void log(const char* format,...) {
    char text[2048]; va_list args; va_start(args,format); const int n=std::vsnprintf(text,sizeof text,format,args); va_end(args);
    api->log(owner,KILN_LOG_INFO,{text,static_cast<size_t>(n<0 ? 0 : n>=static_cast<int>(sizeof text) ? sizeof text-1 : n)});
}
bool status(KilnGameplayStatus& s) { s={}; s.size=sizeof s; return gameplay && gameplay->status(&s)==KILN_OK && s.size>=sizeof s; }
KilnGameplayHookStatus group(const KilnGameplayStatus& s,int i) {
    if(i) return s.hook_groups[i-1];
    KilnGameplayHookStatus result{}; result.state=s.build_hook_state; result.installed_targets=std::popcount(s.build_hook_mask);
    std::snprintf(result.reason,sizeof result.reason,"%s",s.build_hook_reason); return result;
}
int named(const std::string& name) { for(int i=0;i<5;++i) if(name==names[i]) return i; return -1; }
uint32_t mask(int i) { return i==0 || i==2 ? 1u<<KILN_GAMEPLAY_BUILD : i==1 ? 1u<<KILN_GAMEPLAY_CRAFTED :
    i==3 ? 1u<<KILN_GAMEPLAY_SPAWN : (1u<<KILN_GAMEPLAY_ENTITY_JOINED)|(1u<<KILN_GAMEPLAY_ENTITY_LEFT); }
void begin(int i,const KilnGameplayStatus& before) {
    if(i<0) return;
    auto& p=progress[i]; if(p.active) return;
    p.active=true; p.began=pres; p.waiting=group(before,i).state==2;
}
uint32_t KILN_CALL event(KilnGameplayEvent* e,void*) {
    int i=e->kind==KILN_GAMEPLAY_CRAFTED ? 1 : e->kind==KILN_GAMEPLAY_BUILD ? (e->op==KILN_GAMEPLAY_BUILD_TERRAFORM ? 2 : 0) :
        e->kind==KILN_GAMEPLAY_SPAWN ? 3 : e->kind==KILN_GAMEPLAY_ENTITY_JOINED || e->kind==KILN_GAMEPLAY_ENTITY_LEFT ? 4 : -1;
    KilnGameplayStatus s{};
    if(i>=0 && status(s)) {
        if(group(s,i).state==1) { ++progress[i].events;
            if(e->kind==KILN_GAMEPLAY_ENTITY_JOINED) ++joined;
            if(e->kind==KILN_GAMEPLAY_ENTITY_LEFT) ++left;
        } else ++progress[i].early;
        log("gameplay-late: event %s kind %u op %u actor %u target %u id %08x id2 %08x result %d flags %u zone %u at (%.1f, %.1f, %.1f) guid %02x%02x%02x%02x... outputs %u",
            names[i],e->kind,e->op,e->actor,e->target,e->id,e->id2,e->result,e->flags,e->zone,e->position.x,e->position.y,e->position.z,
            e->guid[0],e->guid[1],e->guid[2],e->guid[3],e->output_count);
        if(i==3 && replacement_pending && (e->flags & KILN_GAMEPLAY_F_REPLACED) &&
           std::equal(std::begin(replacement_to),std::end(replacement_to),e->guid)) {
            log("gameplay-late: PASS replacement kept pending, active after (real replaced spawn %u)",e->actor); replacement_pending=false;
        }
    }
    return KILN_GAMEPLAY_KEEP;
}
KilnResult subscribe(int i,uint32_t kinds=0) {
    const uint32_t want=(kinds ? kinds : mask(i)) & ~subscribed;
    if(!want) return KILN_OK;
    KilnHandle h=0; auto rc=gameplay->subscribe(owner,want,&event,nullptr,&h);
    if(rc==KILN_OK) { subscriptions.push_back(h); subscribed |= want; } return rc;
}
void step(const std::string& command,int i,KilnResult rc,const KilnGameplayStatus& before) {
    KilnGameplayStatus after{}; if(!status(after)) { log("gameplay-late: FAIL status unavailable"); return; }
    if(i>=0) {
        auto a=group(before,i),b=group(after,i); progress[i].waiting |= b.state==2;
        log("gameplay-late: step %s API %d; %s state %u targets %u -> state %u targets %u; %s",command.c_str(),rc,names[i],a.state,a.installed_targets,b.state,b.installed_targets,b.reason);
    } else log("gameplay-late: step %s API %d",command.c_str(),rc);
}
bool guid(const std::string& text,uint8_t* out) {
    if(text.size()!=32) return false;
    for(unsigned i=0;i<16;++i) { auto byte=text.substr(i*2,2); if(byte.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos) return false;
        out[i]=static_cast<uint8_t>(std::stoul(byte,nullptr,16)); }
    return true;
}
void command(const std::string& text) {
    std::istringstream input(text); std::string cmd,what; input >> cmd >> what;
    KilnGameplayStatus before{}; if(!status(before)) { log("gameplay-late: FAIL kiln.gameplay status tail unavailable"); return; }
    int i=cmd=="joined" || cmd=="left" ? 4 : named(cmd);
    if(i>=0) { begin(i,before);
        if(i==0) begin(2,before); else if(i==2) begin(0,before); // BUILD interest includes both producer groups
        auto rc=subscribe(i,cmd=="joined" ? 1u<<KILN_GAMEPLAY_ENTITY_JOINED : cmd=="left" ? 1u<<KILN_GAMEPLAY_ENTITY_LEFT : 0);
        if(i==4) log("gameplay-late: entity_filter all API %d",gameplay->entity_filter(KILN_GAMEPLAY_ENTITY_ALL));
        step(text,i,rc,before);
        if(i==0) step(text,2,rc,before); else if(i==2) step(text,0,rc,before);
        return; }
    if(cmd=="status") {
        for(i=0;i<5;++i) { auto g=group(before,i); log("gameplay-late: %s state %u targets %u: %s",names[i],g.state,g.installed_targets,g.reason); } return;
    }
    if(cmd=="unsubscribe") {
        KilnResult result=KILN_OK;
        for(auto h:subscriptions) { auto rc=gameplay->unsubscribe(h); log("gameplay-late: unsubscribe API %d",rc); if(rc!=KILN_OK) result=rc; }
        const bool had_entities=(subscribed & ((1u<<KILN_GAMEPLAY_ENTITY_JOINED)|(1u<<KILN_GAMEPLAY_ENTITY_LEFT)))!=0;
        subscriptions.clear(); subscribed=0;
        if(had_entities) log("gameplay-late: entity_filter idle API %d",gameplay->entity_filter(0));
        KilnGameplayStatus after{}; status(after);
        bool unchanged=result==KILN_OK; for(i=0;i<5;++i) { unchanged &= group(before,i).state==group(after,i).state;
            if(progress[i].active) step(text,i,result,before); }
        log("gameplay-late: %s unsubscribe leaves installed/pending hooks in place",unchanged ? "PASS" : "FAIL"); return;
    }
    if(cmd=="verify") {
        i=named(what); if(i<0) { log("gameplay-late: FAIL unknown verify group"); return; }
        const auto& p=progress[i];
        log("gameplay-late: %s no event before installation, %llu after (%s, early %llu)",p.early==0 && p.events>0 && (i!=4 || (joined && left)) ? "PASS" : "FAIL",
            static_cast<unsigned long long>(p.events),names[i],static_cast<unsigned long long>(p.early));
        if(i==4) log("gameplay-late: entity payload counts joined %llu left %llu",static_cast<unsigned long long>(joined),static_cast<unsigned long long>(left));
        return;
    }
    if(cmd=="zone-remove") {
        auto rc=zone_id ? gameplay->zone_remove(zone_id) : KILN_ERR_NOT_FOUND; zone_id=0; zone_waiting=false; step(text,zone_group,rc,before); return;
    }
    if(cmd=="zone") {
        const char* denies[]={"build","break","terraform","spawn","craft","interact","loot"};
        const uint32_t bits[]={KILN_GAMEPLAY_DENY_BUILD,KILN_GAMEPLAY_DENY_BREAK,KILN_GAMEPLAY_DENY_TERRAFORM,KILN_GAMEPLAY_DENY_SPAWN,
            KILN_GAMEPLAY_DENY_CRAFT,KILN_GAMEPLAY_DENY_INTERACT,KILN_GAMEPLAY_DENY_LOOT};
        int d=-1; for(int j=0;j<7;++j) if(what==denies[j]) d=j;
        if(d<0 || zone_id || zone_waiting) { log("gameplay-late: FAIL unknown deny or previous zone still active"); return; }
        zone={}; zone.size=sizeof zone; zone.shape=KILN_GAMEPLAY_ZONE_SPHERE; zone.radius=5; zone.allowed_steam_ids=&no_owners;
        zone.deny=bits[d]; input >> zone.center.x >> zone.center.y >> zone.center.z;
        zone_deny=bits[d]; zone_group=d<=1 ? 0 : d==2 ? 2 : d==3 ? 3 : -1;
        if(zone_group>=0) begin(zone_group,before);
        auto rc=gameplay->zone_add(owner,&zone,&zone_id); step(text,zone_group,rc,before);
        KilnGameplayStatus after{}; status(after);
        if(zone_group>=0 && group(after,zone_group).state==2) {
            zone_waiting=rc==KILN_ERR_UNAVAILABLE && !zone_id && before.zones==after.zones;
            log("gameplay-late: %s pending zone refused without storage",zone_waiting ? "PASS" : "FAIL");
        }
        return;
    }
    if(cmd=="replace-clear") { uint32_t removed=0; auto rc=gameplay->spawn_replace_clear(owner,&removed);
        log("gameplay-late: replacement clear API %d removed %u",rc,removed); step(text,3,rc,before); replacement_pending=false; return; }
    if(cmd=="replace") {
        std::string to; input >> to; uint8_t from[16]{};
        if(!guid(what,from) || !guid(to,replacement_to)) { log("gameplay-late: FAIL replace needs two GUIDs as 32 hex digits in memory order"); return; }
        begin(3,before); auto rc=gameplay->spawn_replace(owner,from,replacement_to,1.f); step(text,3,rc,before);
        KilnGameplayStatus after{}; status(after); replacement_pending=rc==KILN_OK && group(after,3).state==2;
        if(!replacement_pending) log("gameplay-late: FAIL replacement did not enter pending (use a fresh process)");
        subscribe(3); return;
    }
    if(cmd=="force-unchecked") {
        unsigned target=0; input >> target; i=named(what);
        if(i<0 || target>=(i==0 ? 5u : i==4 ? 2u : 1u) || !std::filesystem::exists(control / "gameplay.lab")) {
            log("gameplay-late: FAIL force-unchecked requires gameplay.lab and a valid group/zero-based target"); return;
        }
        std::ofstream file(control / "gameplay.unchecked"); file << what << ' ' << target; file.close();
        log("gameplay-late: armed unchecked %s target %u; issue its demand command next",what.c_str(),target); return;
    }
    log("gameplay-late: FAIL unknown command %s",text.c_str());
}
// Step intervals around a late install: kiln.gameplay's Pre runs before this one, so a write in step N shows in the
// interval this Pre reads at step N+1. The median of the steps before the command is the baseline.
std::array<double,64> intervals{}; unsigned interval_n=0; int trace=0;
void baseline() {
    const unsigned n=std::min<unsigned>(interval_n,static_cast<unsigned>(intervals.size()));
    if(!n) return;
    std::array<double,64> v=intervals; std::sort(v.begin(),v.begin()+n);
    log("gameplay-late: step interval before the command: median %.1f ms, max %.1f ms over %u steps",v[n/2],v[n-1],n);
}
void KILN_CALL pre(const KilnTickInfo* tick,void*) {
    if(!enabled || !gameplay) return;
    ++pres;
    const double interval=tick && tick->size>=offsetof(KilnTickInfo,wall_delta_ms)+sizeof(double) ? tick->wall_delta_ms : 0;
    if(trace>0) { --trace; log("gameplay-late: step %llu interval %.1f ms",static_cast<unsigned long long>(tick ? tick->step : pres),interval); }
    // Commands are atomic-renamed by the coordinator. Bound input; consume once on the tick thread. Not a *.cmd file:
    // Kiln.Host's control reader takes every *.cmd in this folder as one of its own commands.
    std::ifstream file(control / "gameplay-late.command");
    if(file) {
        char line[513]{}; file.getline(line,sizeof line); const bool valid=!file.fail(); file.close();
        std::error_code ec; std::filesystem::remove(control / "gameplay-late.command",ec);
        baseline(); trace=3;
        if(valid) command(line); else log("gameplay-late: FAIL command exceeds 511 bytes");
    }
    if(interval>0) { intervals[interval_n++ % intervals.size()]=interval; }
    KilnGameplayStatus s{}; if(!status(s)) return;
    bool open=false;
    for(int i=0;i<5;++i) open |= progress[i].active && !progress[i].done;
    if(open) trace=3;  // every step while a group waits, and three more after the last install
    for(int i=0;i<5;++i) {
        auto& p=progress[i]; auto g=group(s,i); if(!p.active || p.done) continue;
        p.waiting |= g.state==2;
        if(g.state==1) { p.done=true;
            log("gameplay-late: %s %s waiting then installed after %llu Pre(s)",p.waiting ? "PASS" : "FAIL",names[i],static_cast<unsigned long long>(pres-p.began)); }
        else if(g.state==3) { p.done=true;
            const bool three=std::string(g.reason).find("three attempts")!=std::string::npos;
            log("gameplay-late: %s %s refused after %s: %s; nothing written past target %u",three ? "PASS" : "FAIL",names[i],three ? "3 attempts" : "unexpected failure",g.reason,g.installed_targets); }
    }
    if(zone_waiting && group(s,zone_group).state==1) {
        auto rc=gameplay->zone_add(owner,&zone,&zone_id); zone_waiting=false;
        log("gameplay-late: %s zone with %u refused while waiting, accepted after (API %d, zone %u)",rc==KILN_OK && zone_id ? "PASS" : "FAIL",zone_deny,rc,zone_id);
    }
}
void KILN_CALL ready(void*) {
    gameplay=static_cast<const KilnGameplayApi*>(api->service({KILN_GAMEPLAY_SERVICE,sizeof(KILN_GAMEPLAY_SERVICE)-1},1));
    if(!gameplay || gameplay->header.size<sizeof(KilnGameplayApi)) { log("gameplay-late: FAIL new gameplay table required"); return; }
    char path[4096]{}; size_t n=0;
    if(!api->path || api->path(KILN_PATH_CONTROL,path,sizeof path,&n)!=KILN_OK || !n || n>=sizeof path) return;
    control=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path),n));
    enabled=std::filesystem::exists(control / "gameplay-late.lab");
    if(!enabled) { log("gameplay-late: disabled (needs gameplay-late.lab at load)"); return; }
    KilnHandle h=0; auto rc=api->subscribe_tick(owner,KILN_TICK_PRE,&pre,nullptr,&h);
    log("gameplay-late: ready; passive until control command (tick API %d)",rc);
}
}
extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api,KilnOwner owner,KilnPluginInfo* info) {
    late::api=api; late::owner=owner; info->id={"gameplay-late",13}; info->version={"1",1}; info->on_ready=&late::ready;
    return KILN_OK;
}
