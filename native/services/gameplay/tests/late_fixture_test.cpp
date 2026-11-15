// SPDX-License-Identifier: MIT
// Exercise the actual lab command reader and PASS criteria without a game. Service installation is covered separately.
#include "../../../samples/gameplay-late/plugin.cpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
static KilnGameplayStatus snapshot{}; static unsigned demands=0,checks=0,failed=0; static std::vector<std::string> lines;
#define CHECK(x) do { ++checks; if(!(x)) { ++failed; std::printf("FAIL %d %s\n",__LINE__,#x); } } while(0)
static bool has(const char* text) { return std::any_of(lines.begin(),lines.end(),[&](const auto& s){return s.find(text)!=std::string::npos;}); }
int main() {
    const auto root=std::filesystem::path(std::getenv("KILN_OUT")) / "gameplay-late-fixture";
    std::filesystem::create_directories(root);
    std::ofstream(root / "gameplay-late.lab").close(); std::ofstream(root / "gameplay.lab").close();
    static auto path=root.string();
    static KilnGameplayApi gp{}; gp.header={sizeof gp,1};
    gp.status=[](KilnGameplayStatus* s)->KilnResult { *s=snapshot; s->size=sizeof *s; return KILN_OK; };
    gp.subscribe=[](KilnOwner,uint32_t mask,KilnGameplayFn,void*,KilnHandle* out)->KilnResult {
        ++demands; *out=demands;
        if(mask & (1u<<KILN_GAMEPLAY_CRAFTED)) snapshot.hook_groups[0].state=2;
        if(mask & (1u<<KILN_GAMEPLAY_SPAWN)) snapshot.hook_groups[2].state=2;
        if(mask & (1u<<KILN_GAMEPLAY_ENTITY_JOINED)) snapshot.hook_groups[3].state=2;
        return KILN_OK;
    };
    gp.entity_filter=[](uint32_t categories)->KilnResult { snapshot.entity_categories=categories; return KILN_OK; };
    gp.unsubscribe=[](KilnHandle)->KilnResult { return KILN_OK; };
    gp.zone_add=[](KilnOwner,const KilnGameplayZone*,uint32_t* id)->KilnResult {
        if(snapshot.build_hook_state!=1) { snapshot.build_hook_state=2; return KILN_ERR_UNAVAILABLE; }
        *id=7; ++snapshot.zones; return KILN_OK;
    };
    gp.zone_remove=[](uint32_t)->KilnResult { --snapshot.zones; return KILN_OK; };
    gp.spawn_replace=[](KilnOwner,const uint8_t*,const uint8_t*,float)->KilnResult { snapshot.hook_groups[2].state=2; return KILN_OK; };
    gp.spawn_replace_clear=[](KilnOwner,uint32_t* removed)->KilnResult { *removed=1; return KILN_OK; };
    KilnApi api{}; api.service=[](KilnStr,uint32_t)->const void* { return &gp; };
    api.log=[](KilnOwner,int32_t,KilnStr text) { lines.emplace_back(text.ptr,text.len); };
    api.path=[](int32_t,char* out,size_t cap,size_t* n)->KilnResult { *n=path.size(); if(cap<*n) return KILN_ERR_BUFFER_TOO_SMALL; std::memcpy(out,path.data(),*n); return KILN_OK; };
    api.subscribe_tick=[](KilnOwner,int32_t,KilnTickFn,void*,KilnHandle* h)->KilnResult { *h=1; return KILN_OK; };
    KilnPluginInfo info{}; CHECK(kiln_plugin_main(&api,77,&info)==KILN_OK); info.on_ready(nullptr); CHECK(demands==0);
    // Kiln.Host's control reader owns every *.cmd in this folder: the fixture must leave such a file alone.
    std::ofstream(root / "gameplay-late.cmd") << "craft\n"; late::pre(nullptr,nullptr);
    CHECK(demands==0); CHECK(std::filesystem::exists(root / "gameplay-late.cmd")); std::filesystem::remove(root / "gameplay-late.cmd");
    auto send=[&](const char* cmd) { std::ofstream(root / "gameplay-late.command") << cmd << '\n'; late::pre(nullptr,nullptr); };
    send("craft"); CHECK(demands==1); CHECK(has("state 0 targets 0 -> state 2")); CHECK(!has("PASS craft waiting then installed"));
    snapshot.hook_groups[0].state=1; KilnTickInfo tick{}; tick.size=sizeof tick; tick.step=41; tick.wall_delta_ms=93.5;
    late::pre(&tick,nullptr); CHECK(has("PASS craft waiting then installed after 1 Pre(s)"));
    CHECK(has("gameplay-late: step 41 interval 93.5 ms"));
    KilnGameplayEvent ev{}; ev.kind=KILN_GAMEPLAY_CRAFTED; late::event(&ev,nullptr); send("verify craft"); CHECK(has("PASS no event before installation, 1 after"));
    send("zone build 1 2 3"); CHECK(snapshot.zones==0); CHECK(has("PASS pending zone refused without storage"));
    snapshot.build_hook_state=1; snapshot.build_hook_mask=31; late::pre(nullptr,nullptr); CHECK(snapshot.zones==1);
    CHECK(has("PASS zone with 1 refused while waiting, accepted after")); send("zone-remove"); CHECK(snapshot.zones==0);
    send("replace 00000000000000000000000000000000 00000000000000000000000000000000"); CHECK(late::replacement_pending);
    snapshot.hook_groups[2].state=1; late::pre(nullptr,nullptr);
    CHECK(!has("PASS replacement kept pending")); // status/registration alone must NEVER certify a live replacement
    ev.kind=KILN_GAMEPLAY_SPAWN; ev.flags=KILN_GAMEPLAY_F_REPLACED; late::event(&ev,nullptr); CHECK(has("PASS replacement kept pending, active after"));
    send("unsubscribe"); CHECK(has("PASS unsubscribe leaves installed/pending hooks in place"));
    send("force-unchecked entities 1"); CHECK(std::filesystem::exists(root / "gameplay.unchecked"));
    send("joined"); CHECK(snapshot.entity_categories==KILN_GAMEPLAY_ENTITY_ALL);
    CHECK((late::subscribed & (1u<<KILN_GAMEPLAY_ENTITY_LEFT))==0);
    send("left"); CHECK((late::subscribed & (1u<<KILN_GAMEPLAY_ENTITY_LEFT))!=0);
    const auto prior=demands; send("left"); CHECK(demands==prior);
    snapshot.hook_groups[3].state=3; snapshot.hook_groups[3].installed_targets=1;
    std::snprintf(snapshot.hook_groups[3].reason,512,"refused: thread check refused on three attempts");
    late::pre(nullptr,nullptr); CHECK(has("PASS entities refused after 3 attempts:"));
    CHECK(has("nothing written past target 1"));
    send("unsubscribe"); CHECK(snapshot.entity_categories==0);
    std::filesystem::remove_all(root);
    std::printf("gameplay late fixture: %u checks, %u failures\n",checks,failed); return failed ? 1 : 0;
}
