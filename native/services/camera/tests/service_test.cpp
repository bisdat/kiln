// SPDX-License-Identifier: MIT
// The actual service entry point and table on a host: no DLL/Wine or game. Exercises wiring, not another Core rig.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include "services/kiln_camera.h"
#include "services/kiln_caps.h"
#include "services/kiln_hooks.h"
#include "services/kiln_players.h"
#include "services/kiln_world.h"
#include "../seh.h"
extern "C" KilnResult KILN_CALL kiln_plugin_main(const KilnApi*, KilnOwner, KilnPluginInfo*);
namespace kcam {
bool seh_done(KilnCameraDone fn, const KilnCameraResult* r, void* u) { fn(r,u); return true; }
bool seh_changed(KilnCameraChanged fn, const KilnCameraChange* r, void* u) { fn(r,u); return true; }
}
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL service:%d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
namespace {
constexpr KilnOwner service_owner=10, author=20;
KilnWorldApi world{}; KilnPlayersApi players{}; KilnHooksApi hooks{}; KilnCapsApi caps{};
struct Row { uint32_t state=0, reason=0; };
std::map<std::string,Row> rows;
KilnResult declare_rc=KILN_OK; uint32_t write_state=KILN_WORLD_WRITES_READY; bool drop=false;
const KilnCameraApi* camera;
KilnWorldEventFn lifecycle; void* life_user;
KilnTickFn pre; void* pre_user;
KilnOwnerRetiringFn retiring; void* retiring_user;
int declared, submits, cancels, done_count, changes;
bool observer=true, old_world=false, live=true, server_thread=true;
uint64_t step=0, incarnation=5, next_ticket=0;
uint8_t ncc[3]{};
struct Job { uint64_t id; KilnWorldWriteRequest q; bool ran=false; };
std::vector<Job> jobs;
std::string str(KilnStr s) { return {s.ptr,s.len}; }
KilnResult KILN_CALL declare(KilnOwner o,KilnStr id,const KilnStr* names,uint32_t n) {
 CHECK(o==service_owner && str(id)=="kiln.camera"); CHECK(n==1 && str(names[0])=="NetworkCameraControl"); ++declared; return declare_rc;
}
KilnResult KILN_CALL info(KilnStr who,uint32_t component,KilnWorldWriteInfo* out) {
 CHECK(str(who)=="kiln.camera" && component==0xd2c927b5u && out && out->size>=sizeof *out); out->state=write_state; return KILN_OK;
}
KilnResult KILN_CALL report(KilnOwner o,KilnStr name,uint32_t state,uint32_t reason,KilnStr) {
 CHECK(o==service_owner); rows[str(name)]=Row{state,reason}; return KILN_OK;
}
KilnResult KILN_CALL submit(KilnOwner o,const KilnWorldWriteRequest* q,uint64_t* out) {
 CHECK(o==service_owner && str(q->mod_id)=="kiln.camera"); CHECK(q->entity==1 && q->component==0xd2c927b5u);
 CHECK(q->change && q->callback && !q->data); *out=++next_ticket; jobs.push_back({*out,*q}); ++submits; return KILN_OK;
}
KilnResult KILN_CALL cancel(uint64_t id) {
 for (auto it=jobs.begin(); it!=jobs.end(); ++it) if(it->id==id) { jobs.erase(it); ++cancels; return KILN_OK; }
 return KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL get(KilnEntity e,KilnPlayer* p) {
 if(e!=1 || !live) return KILN_ERR_NOT_FOUND;
 p->entity=e; p->state=KILN_PLAYER_IN_GAME; p->joined_step=incarnation; p->handle=70; return KILN_OK;
}
KilnResult KILN_CALL subscribe_owner(KilnOwner o,KilnOwnerRetiringFn f,void* u,KilnHandle* h) {
 CHECK(o==service_owner); retiring=f; retiring_user=u; *h=2; return KILN_OK;
}
const void* KILN_CALL service(KilnStr name,uint32_t) {
 auto s=str(name); if(s=="kiln.world") return &world; if(s=="kiln.players") return &players; if(s=="kiln.hooks" && observer) return &hooks; if(s=="kiln.caps") return &caps; return nullptr;
}
KilnResult KILN_CALL publish(KilnOwner o,KilnStr name,const void* t) { CHECK(o==service_owner && str(name)=="kiln.camera"); camera=static_cast<const KilnCameraApi*>(t); return KILN_OK; }
bool KILN_CALL feature(KilnStr s) { const auto name=str(s); return name=="world.lifecycle" || (name=="hooks.owner_cleanup" && observer); }
void KILN_CALL log(KilnOwner,int32_t,KilnStr) {}
uint32_t KILN_CALL side() { return KILN_SIDE_SERVER; }
bool KILN_CALL on_thread(uint32_t kind) { return server_thread && kind==KILN_WORLD_SERVER; }
KilnResult KILN_CALL sub_world(KilnOwner,KilnWorldEventFn fn,void* u,KilnHandle* h) { lifecycle=fn; life_user=u; *h=1; return KILN_OK; }
KilnResult KILN_CALL sub_tick(KilnOwner,uint32_t kind,int32_t phase,KilnTickFn fn,void* u,KilnHandle* h) { CHECK(kind==KILN_WORLD_SERVER && phase==KILN_TICK_PRE); pre=fn; pre_user=u; *h=3; return KILN_OK; }
void KILN_CALL done(const KilnCameraResult* r,void*) { CHECK(r->ok && r->commit_scope==KILN_CAMERA_COMMIT_SERVER_STATE); ++done_count; }
KilnCameraChange last_change{};
void KILN_CALL changed_last(const KilnCameraChange* c,void*) { last_change=*c; ++changes; }
void KILN_CALL changed(const KilnCameraChange*,void*) { ++changes; }
// the transport bridge Kiln.Camera binds (bind_transport): what it was handed
struct BridgeSent { KilnOwner author; uint32_t op; KilnCameraLease lease; uint64_t revision; KilnCameraDone done; void* user; };
std::vector<BridgeSent> bridge_sent;
KilnResult KILN_CALL bridge_submit(KilnOwner a,uint32_t op,KilnCameraLease l,uint64_t rev,const KilnCameraRequest* q,KilnCameraDone d,void* u) {
 CHECK((op==4)==(q==nullptr)); if(q) CHECK(q->endpoint==KILN_CAMERA_REMOTE_VISUAL && q->player_entity==1); bridge_sent.push_back({a,op,l,rev,d,u}); return KILN_OK;
}
KilnResult KILN_CALL bridge_caps(KilnEntity p,uint64_t* m) { *m = p==1 ? KILN_CAMERA_CAP_LOCAL_FOV : 0; return KILN_OK; }
int bridge_retired;
void KILN_CALL bridge_retire(KilnOwner) { ++bridge_retired; }
KilnCameraResult remote_result{}; int remote_done;
void KILN_CALL done_remote(const KilnCameraResult* r,void*) { remote_result=*r; remote_result.reason={}; ++remote_done; }
void event(uint32_t what,uint32_t epoch) { KilnWorldEvent e{}; e.kind=KILN_WORLD_SERVER; e.event=what; e.session=epoch; lifecycle(&e,life_user); }
void tick() {
 auto due=std::move(jobs); jobs.clear();
 for(auto& j:due) { CHECK(j.ran); KilnWorldWriteResult r{}; r.size=sizeof r; r.ticket=j.id; r.status=drop ? KILN_WORLD_WRITE_DROPPED : KILN_WORLD_WRITE_DONE; j.q.callback(&r,j.q.user); }
 KilnTickInfo t{}; t.step=++step; pre(&t,pre_user);
 ncc[0]=0; ncc[1]=7; ncc[2]=0;
 for(auto& j:jobs) { CHECK(!j.ran); if(!drop) CHECK(j.q.change(1,ncc,3,j.q.user)); j.ran=true; }  // an off program runs nothing
 CHECK(jobs.size()<=1);
}
}
int main(int argc,char** argv) {
 const std::string mode_arg=argc>1 ? argv[1] : "";
 observer=mode_arg!="no-observer"; old_world=mode_arg=="old-world";
 const bool refused=mode_arg=="declare-refused", program_off=mode_arg=="program-off";
 if(refused) declare_rc=KILN_ERR_UNAVAILABLE;  // write_declare: tracked writes do not work on this build (sigdb rows missing)
 world.header={old_world ? 8u : uint32_t(sizeof world),KILN_WORLD_VERSION}; world.write_declare=declare; world.write_submit=submit; world.write_cancel=cancel; world.write_info=info;
 caps.header={sizeof caps,KILN_CAPS_VERSION}; caps.report=report;
 players.header={sizeof players,KILN_PLAYERS_VERSION}; players.get=get;
 hooks.header={sizeof hooks,KILN_HOOKS_VERSION}; hooks.subscribe_owner_retiring=subscribe_owner;
 KilnApi api{}; api.size=sizeof api; api.abi_version=KILN_ABI_VERSION; api.service=service; api.register_service=publish; api.feature=feature; api.log=log; api.side=side;
 api.on_world_thread=on_thread; api.subscribe_world=sub_world; api.subscribe_world_tick=sub_tick;
 KilnPluginInfo info{}; CHECK(kiln_plugin_main(&api,service_owner,&info)==KILN_OK); CHECK(pre==nullptr); info.on_ready(nullptr);
 CHECK(camera && camera->header.size==88 && camera->header.version==1);
 uint64_t mask=0; CHECK(camera->capabilities(KILN_CAMERA_SERVER_MODE,0,&mask)==KILN_OK);
 if(!observer || old_world) { CHECK(mask==0); CHECK(submits==0); std::printf("camera service fail-closed: PASS (%d checks)\n",checks); return 0; }
 if(mode_arg=="bridge") {
  // camera.remote: off until Kiln.Camera binds its bridge; on while bound; off again after unbind
  CHECK(rows["camera.remote"].state==KILN_CAP_UNAVAILABLE);
  CHECK(camera->capabilities(KILN_CAMERA_REMOTE_VISUAL,0,&mask)==KILN_OK && mask==0);
  event(KILN_WORLD_STARTED,1);
  KilnCameraRequest q{}; q.size=sizeof q; q.endpoint=KILN_CAMERA_REMOTE_VISUAL; q.player_entity=1; q.parts=KILN_CAMERA_LENS; q.input_policy=KILN_CAMERA_VISUAL_ONLY;
  q.duration_ms=5000; q.lens_source=KILN_CAMERA_LENS_CONSTANT; q.fov_y_degrees=75.0f;
  KilnCameraLease l=0; CHECK(camera->apply(author,&q,&l,done_remote,nullptr)==KILN_ERR_UNAVAILABLE);
  const KilnCameraTransport t{{sizeof(KilnCameraTransport),1},bridge_submit,bridge_caps,bridge_retire};
  CHECK(camera->bind_transport(0,&t)==KILN_ERR_INVALID_ARGUMENT);
  CHECK(camera->bind_transport(900,&t)==KILN_OK);
  CHECK(camera->bind_transport(901,&t)==KILN_ERR_INVALID_ARGUMENT);
  CHECK(rows["camera.remote"].state==KILN_CAP_AVAILABLE);
  CHECK(camera->capabilities(KILN_CAMERA_REMOTE_VISUAL,0,&mask)==KILN_OK && mask==KILN_CAMERA_CAP_REMOTE);
  CHECK(camera->capabilities(KILN_CAMERA_REMOTE_VISUAL,1,&mask)==KILN_OK && mask==(KILN_CAMERA_CAP_REMOTE|KILN_CAMERA_CAP_LOCAL_FOV));
  CHECK(camera->apply(author,&q,&l,done_remote,nullptr)==KILN_OK && l);
  CHECK(bridge_sent.empty()); tick(); CHECK(bridge_sent.size()==1 && bridge_sent[0].author==author && bridge_sent[0].op==1 && bridge_sent[0].lease==l);
  KilnCameraResult r{}; r.size=sizeof r; r.ok=1; r.commit_scope=KILN_CAMERA_COMMIT_CLIENT_FRAME; r.frame=77; bridge_sent[0].done(&r,bridge_sent[0].user);
  CHECK(remote_done==0); tick(); CHECK(remote_done==1 && remote_result.ok==1 && remote_result.commit_scope==KILN_CAMERA_COMMIT_CLIENT_FRAME && remote_result.frame==77 && remote_result.lease==l);
  // a client's own end, through the bridge owner only
  CHECK(camera->transport_ended(901,l,1,KILN_CAMERA_END_EXPIRED,0,KilnStr{"",0})==KILN_ERR_REVOKED);
  KilnHandle sub=0; CHECK(camera->subscribe(30,changed_last,nullptr,&sub)==KILN_OK);
  CHECK(camera->transport_ended(900,l,1,KILN_CAMERA_END_EXPIRED,0,KilnStr{"",0})==KILN_OK);
  tick(); CHECK(changes==1 && last_change.kind==KILN_CAMERA_ENDED && last_change.end_reason==KILN_CAMERA_END_EXPIRED && last_change.lease==l);
  CHECK(camera->unbind_transport(901)==KILN_ERR_REVOKED);
  CHECK(camera->unbind_transport(900)==KILN_OK);
  CHECK(rows["camera.remote"].state==KILN_CAP_UNAVAILABLE);
  CHECK(camera->capabilities(KILN_CAMERA_REMOTE_VISUAL,0,&mask)==KILN_OK && mask==0);
  CHECK(camera->unbind_transport(900)==KILN_ERR_NOT_FOUND);
  event(KILN_WORLD_STOPPING,1);
  std::printf("camera service bridge: PASS (%d checks, camera.remote only while bound)\n",checks); return 0;
 }
 if(refused) {
  CHECK(declared==1); CHECK(mask==0);
  CHECK(rows["camera.server_mode"].state==KILN_CAP_UNAVAILABLE && rows["camera.server_mode"].reason==KILN_CAP_REASON_SERVICE_MISSING);
  event(KILN_WORLD_STARTED,1);
  KilnCameraRequest q{}; q.size=sizeof q; q.endpoint=KILN_CAMERA_SERVER_MODE; q.player_entity=1; q.parts=KILN_CAMERA_MODE; q.input_policy=KILN_CAMERA_STOCK_MODE; q.duration_ms=1000; q.camera_id=34;
  KilnCameraLease l=0; CHECK(camera->apply(author,&q,&l,done,nullptr)==KILN_ERR_UNAVAILABLE);
  tick(); tick(); CHECK(submits==0 && done_count==0);
  std::printf("camera service declare refused: PASS (%d checks)\n",checks); return 0;
 }
 CHECK(declared==1); CHECK(mask==KILN_CAMERA_CAP_SERVER_MODE);
 CHECK(rows["camera.server_mode"].state==KILN_CAP_AVAILABLE);
 event(KILN_WORLD_STARTED,1); KilnHandle sub=0; CHECK(camera->subscribe(30,program_off ? changed_last : changed,nullptr,&sub)==KILN_OK);
 KilnCameraRequest q{}; q.size=sizeof q; q.endpoint=KILN_CAMERA_SERVER_MODE; q.player_entity=1; q.parts=KILN_CAMERA_MODE; q.input_policy=KILN_CAMERA_STOCK_MODE; q.duration_ms=120000; q.camera_id=34; q.transition_override=2;
 KilnCameraLease lease=0; CHECK(camera->apply(author,&q,&lease,done,nullptr)==KILN_OK); CHECK(lease && done_count==0);
 tick(); tick(); CHECK(done_count==1 && changes==1);
 for(int i=0;i<20;++i) { tick(); CHECK(ncc[0]==1 && ncc[1]==34 && ncc[2]==2); }
 CHECK(done_count==1 && changes==1); CHECK(submits==22);
 if(program_off) {
  // kiln.world's program goes off in this world: the next result is DROPPED; the lease ends at once and the row goes off
  write_state=KILN_WORLD_WRITES_OFF; drop=true;
  tick(); tick();
  CHECK(changes==2 && last_change.kind==KILN_CAMERA_ENDED && last_change.end_reason==KILN_CAMERA_END_CAPABILITY_LOST && last_change.lease==lease);
  CHECK(camera->capabilities(KILN_CAMERA_SERVER_MODE,0,&mask)==KILN_OK && mask==0);
  CHECK(rows["camera.server_mode"].state==KILN_CAP_UNAVAILABLE && rows["camera.server_mode"].reason==KILN_CAP_REASON_FAULTED);
  KilnCameraLease l=0; CHECK(camera->apply(author,&q,&l,done,nullptr)==KILN_ERR_UNAVAILABLE);
  // the next server world runs it again: the row comes back
  write_state=KILN_WORLD_WRITES_READY; drop=false;
  event(KILN_WORLD_STOPPING,1); event(KILN_WORLD_STARTED,2); tick();
  CHECK(camera->capabilities(KILN_CAMERA_SERVER_MODE,0,&mask)==KILN_OK && mask==KILN_CAMERA_CAP_SERVER_MODE);
  CHECK(rows["camera.server_mode"].state==KILN_CAP_AVAILABLE);
  event(KILN_WORLD_STOPPING,2);
  std::printf("camera service program off: PASS (%d checks)\n",checks); return 0;
 }
 // Public dispatch requires a connected player, not just an entity with NCC.
 q.player_entity=2; KilnCameraLease absent=0; CHECK(camera->apply(21,&q,&absent,nullptr,nullptr)==KILN_ERR_NOT_FOUND);
 server_thread=false; CHECK(camera->end(author,lease,nullptr,nullptr)==KILN_ERR_WRONG_THREAD); server_thread=true;
 ++incarnation; tick(); CHECK(cancels==1 && jobs.empty() && ncc[0]==0);
 event(KILN_WORLD_STOPPING,1); CHECK(changes==2);
 CHECK(retiring); retiring(author,retiring_user);
 std::printf("camera service: PASS (%d checks, service-owned writes, connected-player incarnation, 20 held steps)\n",checks);
}
