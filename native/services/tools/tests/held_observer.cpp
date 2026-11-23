// SPDX-License-Identifier: MIT
#include "../held_observer.h"
#include <cstdlib>
#include <iostream>
using namespace kiln::tools;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
static KilnClientState state;
static KilnClientApi client;
static KilnTickFn post_callback;
static KilnWorldEventFn world_callback;
static void* post_user;static void* world_user;
static bool on_thread=true,missing=false,change_session=false,refuse_tick=false;
static unsigned client_unsubscribed,world_unsubscribed,reads;
static std::array<uint8_t,216> selection;
static void KILN_CALL get_component(HeldSlice* out,uintptr_t world,uint32_t hash,uint32_t actor) {
    ++reads;*out={};
    if(!missing&&world==123&&hash==0x34bc3e2f&&actor==1)*out={selection.data(),selection.size()};
    if(change_session)++state.generation;
}
static KilnResult KILN_CALL read_state(KilnClientState* out) {*out=state;return KILN_OK;}
static bool KILN_CALL client_thread() {return on_thread;}
static uintptr_t KILN_CALL world() {return 123;}
static KilnResult KILN_CALL subscribe_tick(KilnOwner,int32_t phase,KilnTickFn fn,void* user,KilnHandle* out) {
    CHECK(phase==KILN_TICK_POST);if(refuse_tick)return KILN_ERR_UNAVAILABLE;
    post_callback=fn;post_user=user;*out=11;return KILN_OK;
}
static KilnResult KILN_CALL subscribe_world(KilnOwner,KilnWorldEventFn fn,void* user,KilnHandle* out) {world_callback=fn;world_user=user;*out=12;return KILN_OK;}
static KilnResult KILN_CALL unsubscribe_client(KilnHandle handle) {CHECK(handle==11);++client_unsubscribed;return KILN_OK;}
static KilnResult KILN_CALL unsubscribe_world(KilnHandle handle) {CHECK(handle==12);++world_unsubscribed;return KILN_OK;}
static const void* KILN_CALL service(KilnStr name,uint32_t) {return std::string(name.ptr,name.len)==KILN_CLIENT_SERVICE?&client:nullptr;}
static KilnResult KILN_CALL symbol(KilnStr name,uintptr_t* out) {if(std::string(name.ptr,name.len)!="GetComponent")return KILN_ERR_NOT_FOUND;*out=reinterpret_cast<uintptr_t>(&get_component);return KILN_OK;}
int main() {
    KilnApi api{};api.size=sizeof api;api.service=&service;api.symbol=&symbol;api.subscribe_world=&subscribe_world;api.unsubscribe=&unsubscribe_world;
    client.header={sizeof client,KILN_CLIENT_VERSION};client.state=&read_state;client.subscribe_tick=&subscribe_tick;client.unsubscribe=&unsubscribe_client;client.on_client_thread=&client_thread;client.client_world=&world;
    state.size=sizeof state;state.side=KILN_SIDE_CLIENT;state.session=KILN_SESSION_HOST;state.generation=2;state.client_session=1;state.local_player=1;state.flags=KILN_CLIENT_LOCAL_PLAYER|KILN_CLIENT_LIFECYCLE;state.client_steps=10;
    uint32_t item=3205570361u;std::memcpy(selection.data()+0xb0,&item,4);
    HeldObserver observer;CHECK(!observer.current());CHECK(observer.start(&api,7));CHECK(!observer.current());
    post_callback(nullptr,post_user);CHECK(observer.current()==item&&reads==1);
    selection.fill(0);CHECK(observer.current()==item); // Owned scalar survives mutation of source bytes.
    ++state.client_steps;CHECK(observer.current()==item);++state.client_steps;CHECK(!observer.current());
    std::memcpy(selection.data()+0xb0,&item,4);post_callback(nullptr,post_user);CHECK(observer.current()==item);
    ++state.generation;CHECK(!observer.current());post_callback(nullptr,post_user);CHECK(observer.current()==item);
    ++state.client_session;CHECK(!observer.current());post_callback(nullptr,post_user);CHECK(observer.current()==item);
    state.local_player=2;CHECK(!observer.current());state.local_player=1;
    KilnWorldEvent event{};event.kind=KILN_WORLD_CLIENT;world_callback(&event,world_user);CHECK(!observer.current());
    post_callback(nullptr,post_user);CHECK(observer.current()==item);
    on_thread=false;post_callback(nullptr,post_user);CHECK(!observer.current());on_thread=true;
    missing=true;post_callback(nullptr,post_user);CHECK(!observer.current());missing=false;
    change_session=true;post_callback(nullptr,post_user);CHECK(!observer.current());change_session=false;
    post_callback(nullptr,post_user);CHECK(observer.current()==item);
    observer.stop();CHECK(!observer.current()&&client_unsubscribed==1&&world_unsubscribed==1);
    refuse_tick=true;CHECK(!observer.start(&api,8));CHECK(!observer.current()&&world_unsubscribed==2&&client_unsubscribed==1);
    state.side=KILN_SIDE_SERVER;CHECK(!observer.start(&api,9));
    std::cout<<"held observer: "<<checks<<" checks passed\n";
}
