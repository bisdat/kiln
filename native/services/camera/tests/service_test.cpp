// SPDX-License-Identifier: MIT
// The actual service entry point and table on a host: no DLL/Wine or game. Exercises wiring, not another Core rig.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "services/kiln_camera.h"
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
KilnWorldApi world{}; KilnPlayersApi players{}; KilnHooksApi hooks{};
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
 CHECK(o==service_owner && str(id)=="kiln.camera"); CHECK(n==1 && str(names[0])=="NetworkCameraControl"); ++declared; return KILN_OK;
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
 auto s=str(name); if(s=="kiln.world") return &world; if(s=="kiln.players") return &players; if(s=="kiln.hooks" && observer) return &hooks; return nullptr;
}
KilnResult KILN_CALL publish(KilnOwner o,KilnStr name,const void* t) { CHECK(o==service_owner && str(name)=="kiln.camera"); camera=static_cast<const KilnCameraApi*>(t); return KILN_OK; }
bool KILN_CALL feature(KilnStr s) { const auto name=str(s); return name=="world.lifecycle" || (name=="hooks.owner_cleanup" && observer); }
void KILN_CALL log(KilnOwner,int32_t,KilnStr) {}
uint32_t KILN_CALL side() { return KILN_SIDE_SERVER; }
bool KILN_CALL on_thread(uint32_t kind) { return server_thread && kind==KILN_WORLD_SERVER; }
KilnResult KILN_CALL sub_world(KilnOwner,KilnWorldEventFn fn,void* u,KilnHandle* h) { lifecycle=fn; life_user=u; *h=1; return KILN_OK; }
KilnResult KILN_CALL sub_tick(KilnOwner,uint32_t kind,int32_t phase,KilnTickFn fn,void* u,KilnHandle* h) { CHECK(kind==KILN_WORLD_SERVER && phase==KILN_TICK_PRE); pre=fn; pre_user=u; *h=3; return KILN_OK; }
void KILN_CALL done(const KilnCameraResult* r,void*) { CHECK(r->ok && r->commit_scope==KILN_CAMERA_COMMIT_SERVER_STATE); ++done_count; }
void KILN_CALL changed(const KilnCameraChange*,void*) { ++changes; }
void event(uint32_t what,uint32_t epoch) { KilnWorldEvent e{}; e.kind=KILN_WORLD_SERVER; e.event=what; e.session=epoch; lifecycle(&e,life_user); }
void tick() {
 auto due=std::move(jobs); jobs.clear();
 for(auto& j:due) { CHECK(j.ran); KilnWorldWriteResult r{}; r.size=sizeof r; r.ticket=j.id; r.status=KILN_WORLD_WRITE_DONE; j.q.callback(&r,j.q.user); }
 KilnTickInfo t{}; t.step=++step; pre(&t,pre_user);
 ncc[0]=0; ncc[1]=7; ncc[2]=0;
 for(auto& j:jobs) { CHECK(!j.ran); CHECK(j.q.change(1,ncc,3,j.q.user)); j.ran=true; }
 CHECK(jobs.size()<=1);
}
}
int main(int argc,char** argv) {
 if(argc>1) { observer=std::strcmp(argv[1],"no-observer")!=0; old_world=std::strcmp(argv[1],"old-world")==0; }
 world.header={old_world ? 8u : uint32_t(sizeof world),KILN_WORLD_VERSION}; world.write_declare=declare; world.write_submit=submit; world.write_cancel=cancel;
 players.header={sizeof players,KILN_PLAYERS_VERSION}; players.get=get;
 hooks.header={sizeof hooks,KILN_HOOKS_VERSION}; hooks.subscribe_owner_retiring=subscribe_owner;
 KilnApi api{}; api.size=sizeof api; api.abi_version=KILN_ABI_VERSION; api.service=service; api.register_service=publish; api.feature=feature; api.log=log; api.side=side;
 api.on_world_thread=on_thread; api.subscribe_world=sub_world; api.subscribe_world_tick=sub_tick;
 KilnPluginInfo info{}; CHECK(kiln_plugin_main(&api,service_owner,&info)==KILN_OK); CHECK(pre==nullptr); info.on_ready(nullptr);
 CHECK(camera && camera->header.size==88 && camera->header.version==1);
 uint64_t mask=0; CHECK(camera->capabilities(KILN_CAMERA_SERVER_MODE,0,&mask)==KILN_OK);
 if(!observer || old_world) { CHECK(mask==0); CHECK(submits==0); std::printf("camera service fail-closed: PASS (%d checks)\n",checks); return 0; }
 CHECK(declared==1); CHECK(mask==KILN_CAMERA_CAP_SERVER_MODE);
 event(KILN_WORLD_STARTED,1); KilnHandle sub=0; CHECK(camera->subscribe(30,changed,nullptr,&sub)==KILN_OK);
 KilnCameraRequest q{}; q.size=sizeof q; q.endpoint=KILN_CAMERA_SERVER_MODE; q.player_entity=1; q.parts=KILN_CAMERA_MODE; q.input_policy=KILN_CAMERA_STOCK_MODE; q.duration_ms=120000; q.camera_id=34; q.transition_override=2;
 KilnCameraLease lease=0; CHECK(camera->apply(author,&q,&lease,done,nullptr)==KILN_OK); CHECK(lease && done_count==0);
 tick(); tick(); CHECK(done_count==1 && changes==1);
 for(int i=0;i<20;++i) { tick(); CHECK(ncc[0]==1 && ncc[1]==34 && ncc[2]==2); }
 CHECK(done_count==1 && changes==1); CHECK(submits==22);
 // Public dispatch requires a connected player, not just an entity with NCC.
 q.player_entity=2; KilnCameraLease absent=0; CHECK(camera->apply(21,&q,&absent,nullptr,nullptr)==KILN_ERR_NOT_FOUND);
 server_thread=false; CHECK(camera->end(author,lease,nullptr,nullptr)==KILN_ERR_WRONG_THREAD); server_thread=true;
 ++incarnation; tick(); CHECK(cancels==1 && jobs.empty() && ncc[0]==0);
 event(KILN_WORLD_STOPPING,1); CHECK(changes==2);
 CHECK(retiring); retiring(author,retiring_user);
 std::printf("camera service: PASS (%d checks, service-owned writes, connected-player incarnation, 20 held steps)\n",checks);
}
