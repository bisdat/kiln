// SPDX-License-Identifier: MIT
#pragma once
#include "held_state.h"
#include "kiln_api.h"
#include "services/kiln_client.h"
#include <atomic>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#endif
namespace kiln::tools {
// Shipped GetComponent ABI (world/world_internal.h). Only invoked from the client Post callback.
struct HeldSlice {const void* pointer;uint64_t size;};
using HeldGetComponent=void(KILN_CALL*)(HeldSlice*,uintptr_t,uint32_t,uint32_t);
inline bool copy_held_component(HeldGetComponent get,uintptr_t world,uint32_t entity,uint32_t hash,void* out,size_t expected) {
#ifdef _WIN32
    __try {
#endif
        HeldSlice slice{};get(&slice,world,hash,entity);
        if(!slice.pointer||slice.size!=expected)return false;
        std::memcpy(out,slice.pointer,expected);return true;
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
#endif
}
class HeldObserver final {
    struct Sample {KilnOwner owner;uint32_t generation,session,actor,item;uint64_t step;};
    std::atomic<KilnOwner> owner_{};
    std::atomic<std::shared_ptr<const Sample>> sample_;
    const KilnApi* api_{};const KilnClientApi* client_{};HeldGetComponent get_{};
    KilnHandle tick_{},world_{};
    static bool live(const KilnClientState& state) {
        return state.side==KILN_SIDE_CLIENT&&state.session!=KILN_SESSION_NONE&&state.local_player&&state.client_session&&
            (state.flags&KILN_CLIENT_LOCAL_PLAYER)&&(state.flags&KILN_CLIENT_LIFECYCLE);
    }
    static bool same(const KilnClientState& a,const KilnClientState& b) {
        return live(a)&&live(b)&&a.generation==b.generation&&a.client_session==b.client_session&&a.local_player==b.local_player;
    }
    static void KILN_CALL post(const KilnTickInfo*,void* user) {static_cast<HeldObserver*>(user)->observe();}
    static void KILN_CALL lifecycle(const KilnWorldEvent* event,void* user) {
        if(event&&event->kind==KILN_WORLD_CLIENT)static_cast<HeldObserver*>(user)->sample_.store({},std::memory_order_release);
    }
    void observe() noexcept {
        const auto owner=owner_.load(std::memory_order_acquire);if(!owner)return;
        try {
            KilnClientState before{};before.size=sizeof before;
            if(!client_->on_client_thread()||client_->state(&before)!=KILN_OK||!live(before)){sample_.store({});return;}
            const uintptr_t world=client_->client_world();if(!world){sample_.store({});return;}
            struct Source final:HeldComponents {
                HeldGetComponent get;uintptr_t world;
                Source(HeldGetComponent function,uintptr_t value):get(function),world(value){}
                bool snapshot(uint32_t entity,uint32_t hash,void* out,size_t size) override {return copy_held_component(get,world,entity,hash,out,size);}
            } source(get_,world);
            const auto item=read_held_item(source,before.local_player);
            KilnClientState after{};after.size=sizeof after;
            if(client_->state(&after)!=KILN_OK||!same(before,after)||client_->client_world()!=world||owner_.load(std::memory_order_acquire)!=owner){sample_.store({});return;}
            sample_.store(std::make_shared<const Sample>(Sample{owner,after.generation,after.client_session,after.local_player,item,after.client_steps}),std::memory_order_release);
        } catch(...) {sample_.store({},std::memory_order_release);}
    }
public:
    bool start(const KilnApi* api,KilnOwner owner) {
        if(!api||!owner||!api->service||!api->symbol||!api->subscribe_world||!api->unsubscribe)return false;
        const auto* client=static_cast<const KilnClientApi*>(api->service({KILN_CLIENT_SERVICE,sizeof(KILN_CLIENT_SERVICE)-1},KILN_CLIENT_VERSION));
        if(!client||client->header.size<offsetof(KilnClientApi,client_world)+sizeof(client->client_world)||!client->state||!client->subscribe_tick||!client->unsubscribe||!client->on_client_thread||!client->client_world)return false;
        uintptr_t get=0;if(api->symbol({"GetComponent",12},&get)!=KILN_OK||!get)return false;
        KilnClientState state{};state.size=sizeof state;
        if(client->state(&state)!=KILN_OK||state.side!=KILN_SIDE_CLIENT||!(state.flags&KILN_CLIENT_LIFECYCLE))return false;
        api_=api;client_=client;get_=reinterpret_cast<HeldGetComponent>(get);owner_.store(owner,std::memory_order_release);
        if(api->subscribe_world(owner,&lifecycle,this,&world_)!=KILN_OK||client->subscribe_tick(owner,KILN_TICK_POST,&post,this,&tick_)!=KILN_OK){stop();return false;}
        return true;
    }
    uint32_t current() const noexcept {
        const auto owner=owner_.load(std::memory_order_acquire);if(!owner)return 0;
        auto sample=sample_.load(std::memory_order_acquire);if(!sample||sample->owner!=owner)return 0;
        KilnClientState state{};state.size=sizeof state;
        if(client_->state(&state)!=KILN_OK||!live(state)||state.generation!=sample->generation||state.client_session!=sample->session||state.local_player!=sample->actor||
            state.client_steps<sample->step||state.client_steps-sample->step>1)return 0;
        return sample->item;
    }
    void stop() {
        owner_.store(0,std::memory_order_release);sample_.store({},std::memory_order_release);
        if(tick_&&client_&&client_->unsubscribe)client_->unsubscribe(tick_);
        if(world_&&api_&&api_->unsubscribe)api_->unsubscribe(world_);tick_=world_=0;
    }
};
} // namespace kiln::tools
