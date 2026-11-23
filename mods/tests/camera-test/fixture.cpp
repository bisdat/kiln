// SPDX-License-Identifier: MIT
// Player-free dedicated-server acceptance. Links the SAME Core/ModeBackend as kiln.camera, with a private resolver
// for two real AutomatedPlayer entities. They are not authenticated players and public dispatch is never relaxed.
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include "kiln_api.h"
#include "services/kiln_hooks.h"
#include "services/kiln_spawn.h"
#include "services/kiln_world.h"
#include "mode.h"
namespace {
#define S(x) KilnStr{x,sizeof(x)-1}
const KilnApi* api; KilnOwner owner, a_owner, b_owner;
const KilnWorldApi* world; const KilnSpawnApi* spawn;
std::atomic<int> results[8];
int phase=0, observations=0, wait_steps=0;
uint64_t clock_ms=1000, step_no=0, identity=1, write_baseline=0, unchanged_before=0;
KilnEntity A=0,B=0; KilnCameraLease lease=0, old_lease=0;
bool failed=false, tombstones=true; uint32_t done_ok=0,cancelled=0,already=0,applied=0,updated=0;
uint32_t stale_world=0,released=0,superseded=0,pending_ended=0,expired=0,retired=0,left=0,world_ended=0,already_owned=0,busy=0;
std::vector<uint32_t> event_order;
void say(const std::string& s) { api->log(owner,KILN_LOG_INFO,{s.data(),s.size()}); }
void fail(const char* what) { failed=true; say(std::string("camera-fixture FAIL ")+what); for(auto& r:results) if(!r.load()) r.store(-1); }
#define REQUIRE(x) do { if(!(x)) { fail(#x); return; } } while(0)
struct Env : kcam::Env {
 bool call_done(KilnCameraDone f,const KilnCameraResult& r,void* u) override { f(&r,u); return true; }
 bool call_changed(KilnCameraChanged f,const KilnCameraChange& c,void* u) override { f(&c,u); return true; }
 void log(int32_t,const std::string& s) override { say(s); }
} env;
kcam::Core core(&env);
struct LabWorld : kcam::ModeWorld {
 uint32_t other_jobs=0; std::map<uint64_t,void*> users;
 KilnResult submit(const KilnWorldWriteRequest& q,uint64_t* t) override { if(q.entity==B) ++other_jobs; auto rc=world->write_submit(owner,&q,t); if(rc==KILN_OK) users[*t]=q.user; return rc; }
 KilnResult cancel(uint64_t t) override { tombstones &= !kcam::ModeBackend::ticket_current(users.at(t)); return world->write_cancel(t); }
 bool player(KilnEntity e,kcam::Incarnation* out) override { if(e!=A && e!=B) return false; KilnEntitySnapshot info{}; info.size=sizeof info; if(spawn->entity_info(e,&info)!=KILN_OK) return false; *out={uint64_t(info.spawn_time), identity}; return true; }
 uint64_t now_ms() override { return clock_ms; }
 uint64_t step() override { return step_no; }
 void log(int32_t,const std::string& s) override { say(s); }
} lab;
kcam::ModeBackend mode(&core,&lab,0xd2c927b5u,"kiln.camera.fixture");
void KILN_CALL done(const KilnCameraResult* r,void* mutation) {
 if(r->ok) { if(r->flags & KILN_CAMERA_ALREADY_ENDED) ++already; else { if(mutation && r->commit_scope!=KILN_CAMERA_COMMIT_SERVER_STATE) fail("successful Mode is not ServerState"); ++done_ok; } }
 else if(r->reason_code==KILN_CAMERA_REASON_CANCELLED) ++cancelled;
 else if(r->reason_code==KILN_CAMERA_REASON_ALREADY_OWNED) ++already_owned;
 else if(r->reason_code==KILN_CAMERA_REASON_BUSY) ++busy;
 else if(r->reason_code==KILN_CAMERA_REASON_STALE_WORLD) ++stale_world;
 else fail("unexpected camera refusal");
}
void KILN_CALL change(const KilnCameraChange* c,void*) {
 event_order.push_back(c->kind==KILN_CAMERA_ENDED ? 100+c->end_reason : c->kind);
 if(c->kind==KILN_CAMERA_APPLIED) ++applied;
 if(c->kind==KILN_CAMERA_UPDATED) ++updated;
 if(c->kind==KILN_CAMERA_ENDED) {
  if(c->end_reason==KILN_CAMERA_END_RELEASED) ++released;
  if(c->end_reason==KILN_CAMERA_END_SUPERSEDED) ++superseded;
  if(c->end_reason==KILN_CAMERA_END_CANCELLED_BEFORE_APPLY) ++pending_ended;
  if(c->end_reason==KILN_CAMERA_END_EXPIRED) ++expired;
  if(c->end_reason==KILN_CAMERA_END_OWNER_RETIRED) ++retired;
  if(c->end_reason==KILN_CAMERA_END_PLAYER_LEFT) ++left;
  if(c->end_reason==KILN_CAMERA_END_WORLD_STOPPED) ++world_ended;
 }
}
void KILN_CALL on_retire(KilnOwner o,void*) { core.owner_retiring(o); }
KilnCameraRequest request(int priority=0,uint32_t duration=120000) { KilnCameraRequest q{}; q.size=sizeof q; q.parts=KILN_CAMERA_MODE; q.endpoint=KILN_CAMERA_SERVER_MODE; q.player_entity=A; q.input_policy=KILN_CAMERA_STOCK_MODE; q.duration_ms=duration; q.camera_id=34; q.transition_override=2; q.priority=priority; return q; }
bool apply(KilnOwner who,int priority=0,uint32_t duration=120000) { auto q=request(priority,duration); return core.apply(who,&q,&lease,done,reinterpret_cast<void*>(1),kcam::kOnServer)==KILN_OK; }
bool end(KilnOwner who) { return core.end(who,lease,done,nullptr,kcam::kOnServer)==KILN_OK; }
bool bytes(KilnEntity e,uint32_t hash,void* out,size_t n) { uint32_t len=0; const void* p=world->component(e,hash,&len); if(len!=n || !p) return false; std::memcpy(out,p,n); return true; }
bool stock(KilnEntity e) { uint8_t n[3]{},s[32]{}; return bytes(e,0xd2c927b5u,n,3) && bytes(e,0x9a1138bcu,s,32) && n[0]==s[0] && n[1]==s[1] && n[2]==s[16]; }
bool held() { uint8_t n[3]{}; return bytes(A,0xd2c927b5u,n,3) && n[0]==1 && n[1]==34 && n[2]==2; }
bool make(KilnEntity* out,int i) { KilnSpawnRequest q{}; q.size=sizeof q; q.flags=KILN_SPAWN_TRANSIENT; q.template_name=S("AutomatedPlayer"); q.position={3840.0+3*i,800,1480}; q.tag=S("camera-fixture"); q.origin=S("camera-test"); return spawn->spawn(&q,out)==KILN_OK; }
void finish() { core.world_stopping(kcam::kServer,core.epoch(kcam::kServer)); mode.step(); if(A) spawn->destroy(A); if(B) spawn->destroy(B); A=B=0; phase=99; say("camera-fixture done; Kiln.Testing owns the acceptance counts"); }
void KILN_CALL pre(const KilnTickInfo* tick,void*) {
 if(failed || phase==99) return;
 step_no=tick->step;
 if(++wait_steps>1500) { fail("fixture did not progress in 1500 server steps"); return; }
 // Prime the same NCC bytes before this step's maintainer: its real tracked write must answer Unchanged.
 if(phase==14 && core.active_revision(lease)==0) { static const uint8_t held_bytes[3]={1,34,2}; KilnWorldWriteRequest q{}; q.size=sizeof q; q.entity=A; q.component=0xd2c927b5u; q.flags=KILN_WORLD_WRITE_WHOLE; q.data=held_bytes; q.length=3; q.mod_id=S("kiln.camera.fixture"); uint64_t t=0; REQUIRE(world->write_submit(owner,&q,&t)==KILN_OK); }
 mode.step(); core.pre(kcam::kServer);
 switch(phase) {
 case 0: {
  KilnWorldWriteInfo info{}; info.size=sizeof info;
  if(world->write_info(S("kiln.camera.fixture"),0xd2c927b5u,&info)!=KILN_OK || (info.flags & KILN_WORLD_INFO_NOT_READY)) return;
  REQUIRE(info.state==KILN_WORLD_WRITES_READY && info.component_size==3 && info.writers>0);
  REQUIRE((info.flags & KILN_WORLD_WROTE_REPLICATED) && std::strstr(info.writer_names,"network_camera_control"));
  REQUIRE(make(&A,0) && make(&B,1));
  say("camera-fixture NCC size=3 replicated writer=network_camera_control; two AutomatedPlayer fixtures, not peers");
  core.world_started(kcam::kServer,1); phase=15; break;
 }
 case 15: {
  uint8_t a[3]{},b[3]{}; kcam::Incarnation ai,bi;
  if(!bytes(A,0xd2c927b5u,a,3) || !bytes(B,0xd2c927b5u,b,3)) return;
  REQUIRE(lab.player(A,&ai) && lab.player(B,&bi));
  char text[240]; std::snprintf(text,sizeof text,"camera-fixture natural A=%u NCC=%u,%u,%u incarnation=%llu B=%u NCC=%u,%u,%u incarnation=%llu",A,a[0],a[1],a[2],static_cast<unsigned long long>(ai.a),B,b[0],b[1],b[2],static_cast<unsigned long long>(bi.a)); say(text);
  REQUIRE(apply(a_owner)); phase=1; break;
 }
 case 2: { auto q=request(); q.transition_override=0; REQUIRE(core.update(a_owner,lease,KILN_CAMERA_RESTART_TIMELINE,&q,done,reinterpret_cast<void*>(1),kcam::kOnServer)==KILN_OK); phase=3; break; }
 case 3: if(updated==1) {
  REQUIRE(applied==1); auto q=request(); KilnCameraLease refused=0;
  REQUIRE(core.apply(a_owner,&q,&refused,done,nullptr,kcam::kOnServer)==KILN_OK);
  REQUIRE(core.apply(b_owner,&q,&refused,done,nullptr,kcam::kOnServer)==KILN_OK);
  old_lease=lease; REQUIRE(apply(b_owner,1)); phase=4;
 } break;
 case 4: if(core.active_revision(lease)==1) {
  REQUIRE(already_owned==1 && busy==1 && superseded==1 && applied==2 && updated==1);
  REQUIRE(event_order.size()>=4 && event_order[event_order.size()-2]==106 && event_order.back()==KILN_CAMERA_APPLIED);
  REQUIRE(mode.tickets()==1); auto before=mode.counters(); write_baseline=before.writes;
  REQUIRE(end(b_owner)); REQUIRE(mode.guard(A).lease==0 && mode.tickets()==0);
  REQUIRE(tombstones && mode.counters().cancels==before.cancels+1); REQUIRE(end(b_owner)); phase=5;
 } break;
 case 6: {
  REQUIRE(released==1 && already==1 && applied==2 && updated==1); results[4]=1;
  REQUIRE(apply(b_owner)); mode.step(); REQUIRE(mode.tickets()==1); write_baseline=mode.counters().writes;
  REQUIRE(end(b_owner)); REQUIRE(mode.guard(A).lease==0 && mode.tickets()==0); phase=7; break;
 }
 case 8: REQUIRE(cancelled==1 && pending_ended==1 && applied==2); results[5]=1; REQUIRE(apply(b_owner)); phase=9; break;
 case 9: if(core.active_revision(lease)==1) { REQUIRE(spawn->destroy(A)==KILN_OK); REQUIRE(make(&A,2)); ++identity; say("camera-fixture replaced AutomatedPlayer incarnation"); phase=10; } break;
 case 10: if(left==1) { REQUIRE(mode.tickets()==0); REQUIRE(apply(b_owner,0,100)); phase=11; } break;
 case 11: if(core.active_revision(lease)==1) { clock_ms+=101; phase=12; } break;
 case 12: if(expired==1) { REQUIRE(apply(a_owner)); phase=13; } break;
 case 13: if(core.active_revision(lease)==1) {
  const auto callbacks=done_ok; api->owner_revoke(a_owner); mode.step(); core.pre(kcam::kServer);
  REQUIRE(retired==1 && done_ok==callbacks && mode.tickets()==0);
  // Exercise stop/start through the production model, without restarting the actual engine or retaining pointers.
  REQUIRE(apply(b_owner)); mode.step(); REQUIRE(mode.tickets()==1); auto writes=mode.counters().writes;
  core.world_stopping(kcam::kServer,1); mode.step(); REQUIRE(mode.tickets()==0 && mode.counters().writes==writes && stale_world==1);
  core.world_started(kcam::kServer,2); results[6]=1;
  unchanged_before=mode.counters().results[KILN_WORLD_WRITE_UNCHANGED]; REQUIRE(apply(b_owner)); phase=14;
 } break;
 case 14: if(core.active_revision(lease)==1) {
  REQUIRE(mode.counters().results[KILN_WORLD_WRITE_UNCHANGED]>unchanged_before);
  auto q=request(); KilnCameraLease refused=0;
  for(uint32_t i=0;i<64;++i) REQUIRE(core.apply(b_owner,&q,&refused,done,nullptr,kcam::kOnServer)==KILN_OK);
  REQUIRE(core.apply(b_owner,&q,&refused,done,nullptr,kcam::kOnServer)==KILN_ERR_BUFFER_TOO_SMALL);
  say("camera-fixture Unchanged=ServerState; pending queue accepted=64, refused=65th; End has reserved capacity");
  REQUIRE(end(b_owner)); finish(); if(!failed) results[7]=1;
 } break;
 default: break;
 }
}
void KILN_CALL post(const KilnTickInfo*,void*) {
 if(failed || phase==99 || !A) return;
 if(phase==1 && core.active_revision(lease)==1) {
  REQUIRE(held() && stock(B)); REQUIRE(lab.other_jobs==0);
  if(++observations==20) { REQUIRE(applied==1 && updated==0); say("camera-fixture maintained observations=20 Applied=1 other-entity jobs=0"); results[0]=1; results[1]=1; phase=2; }
 }
 if(phase==5) { REQUIRE(stock(A) && mode.counters().writes==write_baseline); REQUIRE(applied==2 && updated==1); results[2]=1; results[3]=1; phase=6; }
 if(phase==7) { REQUIRE(stock(A) && mode.counters().writes==write_baseline); phase=8; }
}
void KILN_CALL ready(void*) {
 world=static_cast<const KilnWorldApi*>(api->service(S("kiln.world"),KILN_WORLD_VERSION)); spawn=static_cast<const KilnSpawnApi*>(api->service(S("kiln.spawn"),KILN_SPAWN_VERSION));
 REQUIRE(world && spawn); const KilnStr n=S("NetworkCameraControl"); REQUIRE(world->write_declare(owner,S("kiln.camera.fixture"),&n,1)==KILN_OK);
 a_owner=api->owner_create(S("camera.fixture.a")); b_owner=api->owner_create(S("camera.fixture.b"));
 const auto* hooks=static_cast<const KilnHooksApi*>(api->service(S("kiln.hooks"),2)); KilnHandle h=0;
 REQUIRE(hooks && hooks->header.size>=offsetof(KilnHooksApi,unsubscribe_owner_retiring)+sizeof(void*));
 REQUIRE(hooks->subscribe_owner_retiring(owner,on_retire,nullptr,&h)==KILN_OK);
 core.set_backend(KILN_CAMERA_SERVER_MODE,&mode); core.set_writes(true); mode.set_ready(true); REQUIRE(core.subscribe(owner,change,nullptr,&h)==KILN_OK);
 REQUIRE(api->subscribe_world_tick(owner,KILN_WORLD_SERVER,KILN_TICK_PRE,pre,nullptr,&h)==KILN_OK);
 REQUIRE(api->subscribe_world_tick(owner,KILN_WORLD_SERVER,KILN_TICK_POST,post,nullptr,&h)==KILN_OK);
}
}
extern "C" KILN_EXPORT int KILN_CALL camera_fixture_result(uint32_t index) { return index<8 ? results[index].load() : -1; }
extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* a,KilnOwner o,KilnPluginInfo* info) {
 api=a; owner=o; info->id=S("camera-test-fixture"); info->version=S("1"); info->on_ready=ready; return KILN_OK;
}
