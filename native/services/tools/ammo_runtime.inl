// SPDX-License-Identifier: MIT
// Included in presentation.cpp: shares its one selector detour, original-byte qualification and child owner.
namespace ammo {
using namespace kiln::tools;
const KilnClientApi* client{};
const KilnWorldApi* world_api{};
std::atomic<bool> ready{},faulted{};
kiln::RawHook pick_hook,finalizer_hook;
std::mutex mutex;
thread_local unsigned builder_depth{};
struct Publication {
    KilnOwner owner{};uint32_t kind{},mode{},session{},count{};uint64_t serial{};
    std::array<KilnUiAmmoRow,KUA_CAPACITY> rows{};
};
struct Observation {
    KilnUiAmmoContext value{};uint32_t generation{};
    std::array<SelectorRow,KUA_CAPACITY> rows{};
};
struct Binding {
    Observation original;
    std::shared_ptr<const Publication> publication;
    AmmoComposition composed;
};
Observation observations[2];
std::shared_ptr<const Publication> publications[2];
// An address is only an identity token. Never dereference one kept here: engine views are read only from the
// current hook's argument, and are copied into owned arrays before leaving that invocation.
std::map<uintptr_t,Binding> bindings;
std::unordered_set<uint32_t> catalog[2];
uint32_t catalog_session{};
uint64_t revision{};
std::deque<std::pair<KilnOwner,KilnUiAmmoPick>> pending;
int (*backpack_probe)(uint32_t,uint32_t){}; // private fixture seam; production uses stable ECS snapshots
unsigned index(uint32_t kind){return kind==KUA_KIND_ARROW?0:kind==KUA_KIND_SPELL?1:2;}
bool state(KilnClientState& out) {
    out={};out.size=sizeof out;
    return client&&client->state&&client->state(&out)==KILN_OK&&out.side==KILN_SIDE_CLIENT&&out.session!=KILN_SESSION_NONE&&out.local_player&&out.client_session;
}
bool same(const KilnClientState& s,const Observation& o) {
    return s.client_session==o.value.session&&s.generation==o.generation&&s.local_player==o.value.local_player;
}
bool failed(){return faulted.load(std::memory_order_acquire);}
bool available(){return ready.load(std::memory_order_acquire)&&!faulted.load(std::memory_order_acquire);}
bool live_owner(KilnOwner owner) {
    if(!owner||!api||!api->subscribe_tick||!api->unsubscribe)return false;
    KilnHandle lease=0;const auto r=api->subscribe_tick(owner,KILN_TICK_PRE,&noop_tick,nullptr,&lease);
    if(r==KILN_OK)api->unsubscribe(lease);return r==KILN_OK;
}
template<class T> bool read(T& out,const void* p){return copy_game_value(&out,p,sizeof out);}
uint32_t selection(const SelectorGroup* group) {
    SelectorGroup head{}; // Copy only the scalar fields we use, not arbitrary game capacity.
    if(!group||!copy_game_value(&head,group,24)||!read(head.selected,&group->selected)||!read(head.has_selected,&group->has_selected)||
       !head.has_selected||head.selected>=head.count||head.count>4096||!head.data)return 0;
    uint32_t item=0;return read(item,&head.data[head.selected].item)?item:0;
}
bool write_rows(SelectorGroup* group,const SelectorRow* rows,uint32_t count,uint32_t selected) {
    SelectorRow* data=nullptr;uint64_t capacity=0;
    if(!group||!read(data,&group->data)||!read(capacity,&group->capacity)||count>capacity||count>KUA_CAPACITY||(!data&&count))return false;
    if(count&&!copy_game_value(data,rows,sizeof(SelectorRow)*count))return false;
    uint64_t n=count,at=0;uint8_t found=0;
    for(uint32_t i=0;i<count;++i)if(rows[i].item==selected){at=i;found=1;break;}
    return copy_game_value(&group->count,&n,8)&&copy_game_value(&group->selected,&at,8)&&copy_game_value(&group->has_selected,&found,1);
}
void fault(SelectorGroup* group) {
    if(group)clear_selector_view(group);faulted.store(true,std::memory_order_release);
}
// Bindings outlive the session: vanilla enumeration keeps unmatched rows (count zeroed), so a synthetic row stays in
// its group until that group's next build removes it, and its pick stays suppressed (stale: no event) until then.
void clear_session() {
    for(auto& o:observations)o={};for(auto& p:publications)p.reset();pending.clear();
    catalog[0].clear();catalog[1].clear();catalog_session=0;
}
void KILN_CALL lifecycle(const KilnWorldEvent* e,void*) {
    if(e&&e->kind==KILN_WORLD_CLIENT){std::lock_guard lock(mutex);clear_session();}
}
KilnHandle lifecycle_handle{},report_tick{};
std::atomic<bool> fault_reported{};
void KILN_CALL report_post(const KilnTickInfo*,void*) {if(failed()&&!fault_reported.exchange(true))report_caps();}
// The exact ItemRegistry lookup at Ce94470 uses map+0x38; Ce86770 gives these map field offsets. Copy only
// validated ammunition identities/categories, never a borrowed ItemInfo pointer. Refresh on each new client session.
bool copy_catalog(void* access,uint32_t session) {
    if(catalog_session==session)return true;
    const uint8_t* registry=nullptr;
    if(!access||!read(registry,static_cast<const uint8_t*>(access)+0x50)||!registry)return false;
    const auto* map=registry+0x38;uint64_t capacity=0;const uint64_t* occupied=nullptr;const uint32_t* keys=nullptr;const uint8_t* const* values=nullptr;uint32_t count=0;
    if(!read(capacity,map+0x10)||!capacity||capacity>262144||(capacity&(capacity-1))||!read(count,map+0x54)||count>capacity||
       !read(occupied,map+8)||!read(keys,map+0x28)||!read(values,map+0x40)||!occupied||!keys||!values)return false;
    std::unordered_set<uint32_t> copied[2];uint32_t seen=0;
    for(uint64_t word=0;word<(capacity+63)/64;++word) {
        uint64_t bits=0;if(!read(bits,occupied+word))return false;
        for(unsigned bit=0;bit<64&&word*64+bit<capacity;++bit)if(bits&(uint64_t(1)<<bit)) {
            const auto at=word*64+bit;uint32_t key=0,id=0;const uint8_t* info=nullptr;uint8_t category=0,subtype=0;
            if(!read(key,keys+at)||!key||!read(info,values+at)||!info||!read(id,info)||id!=key||!read(category,info+0xcc)||!read(subtype,info+0xcd))return false;
            ++seen;if(category==8&&(subtype==1||subtype==2))copied[subtype-1].insert(key);
        }
    }
    uint64_t again=0;uint32_t count_again=0;const uint64_t* occupied_again=nullptr;const uint32_t* keys_again=nullptr;const uint8_t* const* values_again=nullptr;
    if(seen!=count||!read(again,map+0x10)||again!=capacity||!read(count_again,map+0x54)||count_again!=count||
       !read(occupied_again,map+8)||occupied_again!=occupied||!read(keys_again,map+0x28)||keys_again!=keys||
       !read(values_again,map+0x40)||values_again!=values)return false;
    catalog[0]=std::move(copied[0]);catalog[1]=std::move(copied[1]);catalog_session=session;return true;
}
Frame::Frame(SelectorGroup* groups,void*,uint8_t only) {
    ++builder_depth;
    uint8_t active=7;if(groups&&only)read(active,reinterpret_cast<const uint8_t*>(groups)+0x2bf8);
    std::lock_guard lock(mutex);
    for(unsigned i=0;i<2;++i) {
        rebuild[i]=groups&&(!only||active==i);if(!rebuild[i])continue;
        publication[i]=publications[i];
        const auto old=bindings.find(reinterpret_cast<uintptr_t>(groups+i));
        if(publication[i]||old!=bindings.end())selected[i]=selection(groups+i);
        if(old!=bindings.end()) {
            // Remove last frame's injected rows before vanilla enumeration can accumulate them as stock.
            if(!write_rows(groups+i,old->second.original.rows.data(),old->second.original.value.game_count,selected[i]))fault(groups+i);
            bindings.erase(old);
        }
    }
}
Frame::~Frame(){--builder_depth;}
void after(Frame& frame,SelectorGroup* groups,void* access) try {
    if(!available()||!groups||!access)return;
    KilnClientState before{};if(!state(before))return;
    const uint32_t* info=nullptr;uint32_t held=0;
    if(!read(info,static_cast<const uint8_t*>(access)+0x78)||(info&&!read(held,info)))return;
    std::lock_guard lock(mutex);
    if(catalog_session&&catalog_session!=before.client_session)clear_session();
    const bool known=copy_catalog(access,before.client_session);
    for(unsigned i=0;i<2;++i)if(frame.rebuild[i]) {
        Observation next{};next.value.size=sizeof next.value;next.value.kind=i?KUA_KIND_SPELL:KUA_KIND_ARROW;
        next.value.session=before.client_session;next.value.local_player=before.local_player;next.value.held_item=held;next.generation=before.generation;
        SelectorRow* data=nullptr;uint64_t count=0,capacity=0;
        if(!read(data,&groups[i].data)||!read(count,&groups[i].count)||!read(capacity,&groups[i].capacity)||count>capacity||count>4096||(!data&&count)) {fault(nullptr);continue;}
        next.value.game_count=uint32_t(std::min<uint64_t>(count,KUA_CAPACITY));
        if(next.value.game_count&&!copy_game_value(next.rows.data(),data,next.value.game_count*sizeof(SelectorRow))){fault(nullptr);continue;}
        for(uint32_t r=0;r<next.value.game_count;++r)std::fill(next.rows[r].source.begin()+5,next.rows[r].source.end(),0);
        next.value.ok=known;next.value.reason=!known?KUA_REASON_CATALOG:count>KUA_CAPACITY?KUA_REASON_LIMIT:KUA_REASON_NONE;
        KilnClientState again{};if(!state(again)||again.generation!=before.generation||again.client_session!=before.client_session||again.local_player!=before.local_player)return;
        auto& previous=observations[i];
        const bool equal=previous.value.game_serial&&same(before,previous)&&previous.value.held_item==held&&previous.value.game_count==next.value.game_count&&
            std::memcmp(previous.rows.data(),next.rows.data(),next.value.game_count*sizeof(SelectorRow))==0&&previous.value.ok==next.value.ok;
        next.value.game_serial=equal?previous.value.game_serial:++revision;
        for(uint32_t r=0;r<next.value.game_count;++r)next.value.game_rows[r]={next.rows[r].item,uint32_t(std::min<uint64_t>(next.rows[r].count,UINT32_MAX)),0,r};
        previous=next;
        const auto& p=frame.publication[i];
        if(!known||!p||p!=publications[i]||p->serial!=next.value.game_serial||p->session!=next.value.session)continue;
        auto composed=compose_ammo(next.rows.data(),next.value.game_count,p->rows.data(),p->count,p->mode,frame.selected[i]?frame.selected[i]:selection(groups+i));
        if(composed.count>capacity){composed.count=uint32_t(std::min<uint64_t>(capacity,KUA_CAPACITY));composed.truncated=true;}
        if(bindings.size()>=16) {
            // Only an earlier session's binding may make room; a current one still guards rows drawn this session.
            const auto stale=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b){return b.second.original.value.session!=before.client_session;});
            if(stale==bindings.end()){previous.value.reason=KUA_REASON_LIMIT;continue;}
            bindings.erase(stale);
        }
        // Reserve suppression bookkeeping before the first game write. Allocation failure must leave vanilla
        // rows, never a synthetic row whose later equip request has no binding to intercept.
        const auto [bound,inserted]=bindings.emplace(reinterpret_cast<uintptr_t>(groups+i),Binding{next,p,std::move(composed)});
        if(!inserted){fault(nullptr);continue;}
        const auto& output=bound->second.composed;
        if(!write_rows(groups+i,output.rows.data(),output.count,frame.selected[i]?frame.selected[i]:selection(groups+i))){fault(groups+i);continue;}
        if(output.truncated)previous.value.reason=KUA_REASON_LIMIT;
    }
}
catch(...) {fault(nullptr);}
// Stable, self-validating client ECS reads work on the frame/UI path too; never use legacy server-only component().
bool component(uint32_t entity,uint32_t hash,void* out,uint32_t size) {
    if(!world_api||!child)return false;
    KilnObjectRef ref{};ref.size=sizeof ref;
    const auto session=world_api->game_world_session(KILN_WORLD_CLIENT);
    uint32_t required=0;
    return session&&world_api->entity_open(child,0,KILN_WORLD_CLIENT,session,entity,&ref)==KILN_OK&&
        world_api->ecs_snapshot(child,&ref,hash,out,size,&required)==KILN_OK&&required==size;
}
int backpack(uint32_t actor,uint32_t item) {
    if(backpack_probe)return backpack_probe(actor,item);
    struct Setup {uint32_t ids[16];uint8_t categories[16],flags,total,available,initialized;};
    static_assert(sizeof(Setup)==84);
    std::array<uint32_t,64> todo{},seen{};uint32_t used=1,visited=0;todo[0]=actor;bool found=false;
    while(used) {
        const auto entity=todo[--used];
        for(uint32_t i=0;i<visited;++i)if(seen[i]==entity)return -1;
        if(visited==seen.size())return -1;seen[visited++]=entity;
        Setup setup{},again{};if(!component(entity,0xc24df82au,&setup,sizeof setup)||!setup.initialized)return -1;
        for(unsigned i=0;i<16&&setup.ids[i];++i) {
            if(setup.categories[i]==5) {if(used==todo.size())return -1;todo[used++]=setup.ids[i];}
            else if(setup.categories[i]==4) {
                uint32_t stacks[24]{};if(!component(setup.ids[i],0xb19528a9u,stacks,sizeof stacks))return -1;
                for(unsigned s=0;s<8;++s)if(stacks[s*3]==item&&stacks[s*3+1])found=true;
            }
        }
        if(!component(entity,0xc24df82au,&again,sizeof again)||std::memcmp(&setup,&again,sizeof setup))return -1;
    }
    return found?1:0;
}
// Direct UI wishes need not have gone through C40c320's index update. Preserve only the picture's
// identity here; do not write the game's pending/equip wish at +63c/+640 or ClientPlayerInput.
bool select_identity(SelectorGroup* group,uint32_t selected) {
    SelectorRow* data=nullptr;uint64_t count=0,capacity=0;
    if(!read(data,&group->data)||!read(count,&group->count)||!read(capacity,&group->capacity)||count>capacity||count>KUA_CAPACITY||(!data&&count)){fault(nullptr);return false;}
    for(uint64_t r=0;r<count;++r) {
        uint32_t item=0;if(!read(item,&data[r].item)){fault(nullptr);return false;}
        if(item==selected) {
            const uint8_t yes=1;
            if(!copy_game_value(&group->selected,&r,8)||!copy_game_value(&group->has_selected,&yes,1)){fault(group);return false;}
            return true;
        }
    }
    return false;
}
void KILN_CALL pick(SelectorGroup* groups,void* input,uint8_t category,uint32_t item,uint32_t version) try {
    kt::ToolPolicyScope no_impact_policy(nullptr);
    std::shared_ptr<const Publication> p;Observation original{};KilnUiAmmoRow picked{};bool injected=false;
    if(groups&&category<2) {
        std::lock_guard lock(mutex);const auto b=bindings.find(reinterpret_cast<uintptr_t>(groups+category));
        if(b!=bindings.end())for(uint32_t i=0;i<b->second.composed.count;++i)if(b->second.composed.rows[i].item==item) {
            p=b->second.publication;original=b->second.original;
            for(uint32_t j=0;j<p->count;++j)if(p->rows[j].item==item){picked=p->rows[j];injected=true;break;}
            // A row the game listed itself is the game's own in either mode (vanilla equips it), whatever a probe says.
            // Not one it kept at count 0: a rebuild zeroes every row it keeps, adds stock only to matching rows and does
            // not remove the rest, so a used-up stack's row stays listed at 0 (issue 131). The backpack below decides it.
            for(uint32_t j=0;j<original.value.game_count;++j)if(original.rows[j].item==item&&original.rows[j].count)injected=false;
            break;
        }
    }
    if(!injected){pick_hook.call<void>(groups,input,category,item,version);return;}
    KilnClientState before{};
    if(!state(before)||!same(before,original))return; // A stale synthetic row is never native equip permission.
    const int actual=backpack(before.local_player,item);
    KilnClientState after{};if(!state(after)||after.generation!=before.generation||after.client_session!=before.client_session||after.local_player!=before.local_player)return;
    if(actual==1){pick_hook.call<void>(groups,input,category,item,version);return;}
    if(actual<0)return;
    std::lock_guard lock(mutex);
    if(publications[category]!=p||!available()||!select_identity(groups+category,item))return;
    if(pending.size()==KUA_CAPACITY)pending.pop_front();
    pending.emplace_back(p->owner,KilnUiAmmoPick{sizeof(KilnUiAmmoPick),p->kind,item,picked.count,picked.row,before.client_session,before.client_steps});
    // Deliberately do not call the continuation: no inventoryStockCycleAction version/item or pending native
    // selection at group+0x63c/+0x640 is written for a non-backpack row. The server half alone performs a swap.
}
catch(...) {fault(nullptr);}
void KILN_CALL finalizer(SelectorGroup* groups,void* a,void* b,void* c,void* d,void* e) {
    kt::ToolPolicyScope no_impact_policy(nullptr);
    uint32_t selected[2]{};
    if(groups&&!builder_depth) {std::lock_guard lock(mutex);for(unsigned i=0;i<2;++i)if(bindings.contains(reinterpret_cast<uintptr_t>(groups+i)))selected[i]=selection(groups+i);}
    finalizer_hook.call<void>(groups,a,b,c,d,e);
    if(groups)for(unsigned i=0;i<2;++i)if(selected[i]) {
        std::lock_guard lock(mutex);const auto at=bindings.find(reinterpret_cast<uintptr_t>(groups+i));
        if(at==bindings.end())continue;
        select_identity(groups+i,selected[i]);
    }
}
KilnResult KILN_CALL publish(KilnOwner owner,const KilnUiAmmoPublish* value) {
    if(!value||value->size<sizeof *value||index(value->kind)>1||value->mode>KUA_MODE_REPLACE||value->reserved||value->count>KUA_CAPACITY||(!value->rows&&value->count))return KILN_ERR_INVALID_ARGUMENT;
    if(!live_owner(owner))return owner?KILN_ERR_REVOKED:KILN_ERR_INVALID_ARGUMENT;
    if(!available())return KILN_ERR_UNAVAILABLE;
    if(!client->on_client_thread())return KILN_ERR_WRONG_THREAD;
    try {
        const unsigned i=index(value->kind);auto p=std::make_shared<Publication>();p->owner=owner;p->kind=value->kind;p->mode=value->mode;p->session=value->session;p->serial=value->serial;p->count=value->count;
        if(value->count)std::copy_n(value->rows,value->count,p->rows.data());
        std::lock_guard lock(mutex);
        if(!live_owner(owner))return KILN_ERR_REVOKED;
        if(publications[i]&&publications[i]->owner!=owner)return KUA_ERR_BUSY;
        if(value->mode==KUA_MODE_GAME){publications[i].reset();pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const auto& e){return e.first==owner&&e.second.kind==value->kind;}),pending.end());return KILN_OK;}
        KilnClientState s{};const auto& context=observations[i];
        if(!context.value.game_serial)return KILN_ERR_NOT_FOUND;
        if(!state(s)||!same(s,context)||value->session!=s.client_session||value->serial!=context.value.game_serial)return KILN_ERR_INVALID_ARGUMENT;
        if(!context.value.ok||catalog_session!=s.client_session)return KILN_ERR_UNAVAILABLE;
        std::unordered_set<uint32_t> seen;
        for(uint32_t r=0;r<p->count;++r)if(p->rows[r].flags||!catalog[i].contains(p->rows[r].item)||!seen.insert(p->rows[r].item).second)return KILN_ERR_INVALID_ARGUMENT;
        // Pick keys belong to the old publication. A replacement cannot retarget a queued wish to a new provider.
        pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const auto& e){return e.first==owner&&e.second.kind==value->kind;}),pending.end());
        publications[i]=std::move(p);return KILN_OK;
    }catch(...){return KILN_ERR_UNAVAILABLE;}
}
KilnResult KILN_CALL context(KilnOwner owner,uint32_t kind,KilnUiAmmoContext* out) {
    if(!out||out->size<sizeof *out||index(kind)>1||!owner)return KILN_ERR_INVALID_ARGUMENT;
    *out={};out->size=sizeof *out;out->kind=kind;
    if(!live_owner(owner))return KILN_ERR_REVOKED;
    if(!available())return KILN_ERR_UNAVAILABLE;
    std::lock_guard lock(mutex);KilnClientState s{};
    if(!state(s)){out->reason=KUA_REASON_NO_SESSION;return KILN_ERR_NOT_FOUND;}
    const auto& observed=observations[index(kind)];
    if(!observed.value.game_serial){out->reason=KUA_REASON_NO_CONTEXT;return KILN_ERR_NOT_FOUND;}
    if(!same(s,observed)){out->reason=KUA_REASON_STALE;return KILN_ERR_NOT_FOUND;}
    *out=observed.value;return KILN_OK;
}
KilnResult KILN_CALL take_picks(KilnOwner owner,KilnUiAmmoPick* out,uint32_t capacity,uint32_t* written) {
    if(written)*written=0;
    if(!written||!owner||capacity>KUA_CAPACITY||(!out&&capacity))return KILN_ERR_INVALID_ARGUMENT;
    if(!live_owner(owner))return KILN_ERR_REVOKED;
    if(!available())return KILN_ERR_UNAVAILABLE;
    if(!client->on_client_thread())return KILN_ERR_WRONG_THREAD;
    std::lock_guard lock(mutex);KilnClientState s{};const bool live=state(s);
    for(auto i=pending.begin();i!=pending.end();) {
        if(!live||i->second.session!=s.client_session){i=pending.erase(i);continue;}
        if(i->first!=owner||*written==capacity){++i;continue;}
        out[(*written)++]=i->second;i=pending.erase(i);
    }
    return KILN_OK;
}
void retire(KilnOwner owner) {
    std::lock_guard lock(mutex);
    for(auto& p:publications)if(p&&p->owner==owner)p.reset();
    pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const auto& p){return p.first==owner;}),pending.end());
}
bool start() {
    client=static_cast<const KilnClientApi*>(api->service({KILN_CLIENT_SERVICE,sizeof(KILN_CLIENT_SERVICE)-1},KILN_CLIENT_VERSION));
    world_api=static_cast<const KilnWorldApi*>(api->service({KILN_WORLD_SERVICE,sizeof(KILN_WORLD_SERVICE)-1},KILN_WORLD_VERSION));
    if(!client||client->header.size<offsetof(KilnClientApi,client_world)+sizeof(client->client_world)||!client->state||!client->on_client_thread||!client->subscribe_tick||!client->unsubscribe||
       !world_api||world_api->header.size<offsetof(KilnWorldApi,ecs_snapshot)+sizeof(world_api->ecs_snapshot)||!world_api->game_world_session||!world_api->entity_open||!world_api->ecs_snapshot||
       !api->subscribe_world||api->subscribe_world(child,&lifecycle,nullptr,&lifecycle_handle)!=KILN_OK||client->subscribe_tick(child,KILN_TICK_POST,&report_post,nullptr,&report_tick)!=KILN_OK)return false;
    uintptr_t lookup=0,map=0;
    if(api->symbol({"ui/AmmoItemLookup",17},&lookup)!=KILN_OK||api->symbol({"ui/AmmoItemMap",14},&map)!=KILN_OK||
       !qualify(lookup,"89 4C 24 08 48 83 EC 28 85 C9 74 1B 48 8D 4A 38 48 8D 54 24 30 E8 ?? ?? ?? ?? 48 85 C0 74 08 48 8B 00")||
       !qualify(map,"48 89 5C 24 08 48 89 7C 24 10 83 79 54 00 4C 8B C9 48 8B 59 10 74 53 44 8B 1A 48 8D 43 FF 48 8B 79 08 49 23 C3"))return false;
    uint8_t call[5]{};int32_t rel=0;
    if(hooks->read_original(lookup+21,call,5)!=KILN_OK||call[0]!=0xe8)return false;
    std::memcpy(&rel,call+1,4);if(int64_t(lookup)+26+rel!=int64_t(map))return false;
    const bool final=install(finalizer_hook,"ui/AmmoFinalizer","48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 4C 8B 6D 50 49 8B F1 4C 8B 65 58",reinterpret_cast<void*>(&finalizer));
    const bool pick_ok=install(pick_hook,"ui/AmmoPick","4C 8B D9 41 80 F8 07 0F 83 91 00 00 00 48 8D 0D ?? ?? ?? ?? 41 0F B6 C0 8B 84 81 F0 C1 40 00 48 03 C1 FF E0",reinterpret_cast<void*>(&pick));
    ready.store(final&&pick_ok,std::memory_order_release);return available();
}
void stop() {
    ready.store(false,std::memory_order_release);
    if(report_tick&&client&&client->unsubscribe)client->unsubscribe(report_tick);report_tick=0;
    if(lifecycle_handle&&api&&api->unsubscribe)api->unsubscribe(lifecycle_handle);lifecycle_handle=0;
    std::lock_guard lock(mutex);clear_session();
}
const KilnUiAmmoApi table{{sizeof(KilnUiAmmoApi),KILN_UI_AMMO_VERSION},&publish,&context,&take_picks};
} // namespace ammo
