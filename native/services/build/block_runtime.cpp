// SPDX-License-Identifier: MIT
#include "internal.h"
#include "block_runtime.h"
#include "block_ops.h"
#include "services/kiln_caps.h"
#include <map>
#include <limits>
#include "terrain_damage.h"
#include "services/kiln_settings.h"

namespace kb {
namespace {
uintptr_t building_config=0,terrain_config=0;
int64_t hardness_offset=-1,health_offset=-1;
bool metadata_on=false,provenance_on=false,provenance_faulted=false;
BlockBaseFunctions base_functions;
uintptr_t damage_get_fn=0,damage_add_fn=0;
bool damage_on=false,damage_faulted=false,clock_valid=false;
int64_t now_ns=0;uint32_t clock_tick=0;uint64_t damage_session=0;
uintptr_t damage_world=0;
blocks::TerrainDamage terrain_damage;
const KilnSettingsApi* settings=nullptr;
struct Context {
    uintptr_t registry=0,editor=0,damage_store=0;
    std::array<uint64_t,3> maximum{};
    std::map<std::array<int32_t,3>,std::pair<bool,std::array<KilnVoxel,512>>> baked;
};
int32_t provenance(void* p,const int32_t* c,KilnVoxel current) {
    auto& ctx=*static_cast<Context*>(p);
    if(provenance_faulted)return -1;
    std::array<int32_t,3> brick{};
    for(int a=0;a<3;++a)brick[a]=static_cast<int32_t>(std::floor(c[a]/8.0));
    auto it=ctx.baked.find(brick);
    if(it==ctx.baked.end()) {
        std::pair<bool,std::array<KilnVoxel,512>> copy{};
        const int rc=block_base_brick(base_functions,ctx.editor,brick.data(),copy.second.data());
        if(rc<0) {
            provenance_faulted=true;F.write=false;
            set_detail("baked-world read faulted; restart required before further world edits");return -1;
        }
        copy.first=rc==1;it=ctx.baked.emplace(brick,copy).first;
    }
    if(!it->second.first)return -1;
    const size_t index=static_cast<size_t>(c[0]-brick[0]*8)+8*static_cast<size_t>(c[1]-brick[1]*8)+64*static_cast<size_t>(c[2]-brick[2]*8);
    const auto base=it->second.second[index];const auto m=KILN_VOXEL_MATERIAL(current),bm=KILN_VOXEL_MATERIAL(base);
    if(m>=128)return bm==m?1:0;
    return bm>0 && bm<128 && KILN_VOXEL_FILL(base)>0?1:0;
}

uint32_t session_reason() {
    if(!g_api || !g_api->session) return KILN_BUILD_BLOCK_OFFLINE;
    KilnSessionInfo s{};s.size=sizeof s;
    if(g_api->session(&s)!=KILN_OK)return KILN_BUILD_BLOCK_OFFLINE;
    if(s.state==KILN_SESSION_REMOTE)return KILN_BUILD_BLOCK_WRONG_SIDE;
    if(!s.worlds[KILN_WORLD_SERVER] || !s.lifecycle || !world_running())return KILN_BUILD_BLOCK_NO_SESSION;
    return KILN_BUILD_BLOCK_NONE;
}
KilnResult read(void* p,const int32_t* o,const uint32_t* d,KilnVoxel* out,size_t count) {
    const auto& ctx=*static_cast<const Context*>(p);
    // The merged reader can report loaded air outside its horizontal grid (and solid below zero).
    // Those sentinels are not usable world cells: preserve the public Build.Grid bounds explicitly.
    for(int a=0;a<3;++a)
        if(o[a]<0 || static_cast<uint64_t>(o[a])+d[a]>ctx.maximum[a])return KILN_ERR_INVALID_ARGUMENT;
    return vox_read(o,d,out,count);
}
KilnResult write(void*,const KilnBuildWrite* q,KilnBuildResult* r) {
    return vox_write(q,r,KILN_GAMEPLAY_DENY_BUILD|KILN_GAMEPLAY_DENY_TERRAFORM);
}
bool metadata(void* p,uint8_t material,int32_t* h,int32_t* hp) {
    const auto* c=static_cast<const Context*>(p);
    return c->registry && block_config_read(material>=128?building_config:terrain_config,c->registry,
        material>=128?static_cast<uint8_t>(material-128):material,hardness_offset,health_offset,h,hp);
}
bool damage_get(void* p,const int32_t* c,uint8_t material,uint8_t* out) {
    if(!clock_valid || damage_faulted)return false;
    if(material<128){*out=terrain_damage.get(now_ns,{c[0],c[1],c[2]},material);return true;}
    const auto& ctx=*static_cast<Context*>(p);
    if(!ctx.damage_store || !damage_on || c[0]<0 || c[1]<0 || c[2]<0)return false;
    if(block_damage_read(damage_get_fn,ctx.damage_store,c,out))return true;
    damage_faulted=true;F.write=false;set_detail("building damage read faulted; restart required");return false;
}
uint32_t damage_add(void* p,const int32_t* c,uint8_t material,uint8_t amount,double expiry,uint8_t* out) {
    if(!clock_valid || damage_faulted)return KILN_BUILD_BLOCK_FAULTED;
    if(material<128) {
        bool full=false;
        if(terrain_damage.add(now_ns,{c[0],c[1],c[2]},material,amount,expiry,*out,&full))return KILN_BUILD_BLOCK_NONE;
        return full?KILN_BUILD_BLOCK_DAMAGE_MAP_FULL:KILN_BUILD_BLOCK_FAULTED;
    }
    const auto& ctx=*static_cast<Context*>(p);
    if(!ctx.damage_store || !damage_on || c[0]<0 || c[1]<0 || c[2]<0)return KILN_BUILD_BLOCK_FAULTED;
    if(block_damage_apply(damage_add_fn,ctx.damage_store,now_ns,clock_tick,c,amount,out))return KILN_BUILD_BLOCK_NONE;
    damage_faulted=true;F.write=false;set_detail("building damage mutation faulted; restart required");return KILN_BUILD_BLOCK_FAULTED;
}
KilnResult prepare(blocks::Source& src,Context& c,uint32_t& why) {
    if(!tick_thread()){why=KILN_BUILD_BLOCK_WRONG_THREAD;return KILN_ERR_WRONG_THREAD;}
    if(provenance_faulted || damage_faulted){why=KILN_BUILD_BLOCK_FAULTED;return KILN_ERR_UNAVAILABLE;}
    if((why=session_reason())!=KILN_BUILD_BLOCK_NONE)return KILN_ERR_UNAVAILABLE;
    if(!F.read){why=KILN_BUILD_BLOCK_SYMBOL_MISSING;return KILN_ERR_UNAVAILABLE;}
    const auto* v=vox();if(!v){why=KILN_BUILD_BLOCK_NO_SESSION;return KILN_ERR_UNAVAILABLE;}
    src.context=&c;src.cell_size=v->cell;std::memcpy(src.origin,v->origin,sizeof src.origin);
    c.maximum={static_cast<uint64_t>(v->tile[0])*v->tiles[0],8192,static_cast<uint64_t>(v->tile[1])*v->tiles[1]};
    src.read=&read;src.write=F.write?&write:nullptr;
    if(metadata_on) {
        c.registry=g_api->singleton(server_world(),kstr("TerraformingEfficiencyRegistry"));
        if(c.registry)src.metadata=&metadata;
    }
    if(provenance_on){c.editor=v->voxels;src.provenance=&provenance;}
    if(damage_on && clock_valid) {
        c.damage_store=g_api->singleton(server_world(),kstr("ServerVoxelDamageSystem"));
        src.damage_get=&damage_get;
        src.damage_forget=[](void*,const int32_t* cell,uint8_t material){if(material<128)terrain_damage.forget({cell[0],cell[1],cell[2]});};
        if(c.damage_store && settings) {
            uint32_t index=0;KilnSettingValue value{};
            if(settings->find(kstr("miningDamageFactor"),&index)==KILN_OK && settings->get(index,&value)==KILN_OK &&
               value.kind==KILN_SETTING_FLOAT && std::isfinite(value.number) && value.number>0 && value.number<=100) {
                src.mining_factor=value.number;src.damage_add=&damage_add;
            }
        }
    }
    return KILN_OK;
}
template<class T> KilnResult refused(T* out,KilnResult rc,uint32_t why) {
    const uint32_t size=out->size;*out={};out->size=size;out->refusal=why;return rc;
}
void report(const char* name,bool qualified,const char* missing,bool session) {
    const auto* caps=static_cast<const KilnCapsApi*>(g_api->service(kstr(KILN_CAPS_SERVICE),1));
    if(!caps || caps->header.size<offsetof(KilnCapsApi,report)+sizeof(void*) || !caps->report)return;
    uint32_t reason=(provenance_faulted || damage_faulted)?KILN_CAP_REASON_FAULTED:qualified?(session?session_reason():0):KILN_BUILD_BLOCK_SYMBOL_MISSING;
    uint32_t state=reason==KILN_BUILD_BLOCK_NO_SESSION?KILN_CAP_PENDING:reason?KILN_CAP_UNAVAILABLE:KILN_CAP_AVAILABLE;
    const char* text=(provenance_faulted || damage_faulted)?"block engine state faulted; restart required; further world edits refused":!qualified?missing:reason==KILN_BUILD_BLOCK_WRONG_SIDE?"server authority only; ask the host or dedicated server":
        reason==KILN_BUILD_BLOCK_OFFLINE?"no game session provider in this process":
        reason==KILN_BUILD_BLOCK_NO_SESSION?"waiting for the running server world":"server tick only; loaded voxel cells";
    caps->report(g_owner,kstr(name),state,reason,kstr(text));
}
}
void block_world_closed() {
    terrain_damage.clear();damage_session=0;damage_world=0;clock_valid=false;
    // Fault latches are process-wide: another hosted world cannot prove interrupted engine locks recovered.
}
void block_tick(const KilnTickInfo* tick) {
    clock_valid=false;
    if(!tick || tick->clock_period<=0 || tick->clock==UINT32_MAX ||
       tick->clock>static_cast<uint64_t>((INT64_MAX-20000000000LL)/tick->clock_period))return;
    KilnSessionInfo s{};s.size=sizeof s;
    if(!g_api->session || g_api->session(&s)!=KILN_OK || !s.worlds[KILN_WORLD_SERVER] || !s.sessions[KILN_WORLD_SERVER]) {
        block_world_closed();return;
    }
    if(damage_session!=s.sessions[KILN_WORLD_SERVER] || damage_world!=s.worlds[KILN_WORLD_SERVER])terrain_damage.clear();
    damage_session=s.sessions[KILN_WORLD_SERVER];damage_world=s.worlds[KILN_WORLD_SERVER];
    now_ns=tick->clock_period*static_cast<int64_t>(tick->clock);clock_tick=tick->clock;clock_valid=true;
}
void block_ready() {
    auto symbol=[&](const char* n,uintptr_t& p){return g_api->symbol(kstr(n),&p)==KILN_OK && p;};
    auto constant=[&](const char* n,int64_t& v){return g_api->constant(kstr(n),&v)==KILN_OK;};
    metadata_on=g_api->feature(kstr("build.block_metadata")) && symbol("build/BuildingConfigFind",building_config) &&
        symbol("tools/TerrainConfigLookup",terrain_config) && constant("build/block_config.hardness",hardness_offset) &&
        constant("build/block_config.health",health_offset) && hardness_offset==0x14 && health_offset==0x1c;
    provenance_on=g_api->feature(kstr("build.block_provenance")) && symbol("build/BaseColumnAcquire",base_functions.acquire) &&
        symbol("build/BaseColumnSeekBrick",base_functions.seek) && symbol("build/BaseBrickDecode",base_functions.decode) &&
        symbol("build/BaseColumnRelease",base_functions.release);
    for(const auto& entry:std::initializer_list<std::pair<const char*,int64_t>>{
        {"handle_size",0xa0},{"ready",0x90},{"data",0x20},{"loader",0x10},{"id",0x18},{"lock",0},{"lock_mode",8},
        {"seek",0x30},{"uniform",0x80},{"material",0x68},{"fill",0x70},{"compressed",0x50},{"end",0x60},{"size",0x88},{"decode_count",512}}) {
        int64_t actual=-1;std::string name="build/base_column.";name+=entry.first;
        provenance_on=constant(name.c_str(),actual) && actual==entry.second && provenance_on;
    }
    damage_on=g_api->feature(kstr("build.block_damage")) && symbol("build/VoxelDamageGet",damage_get_fn) &&
        symbol("build/VoxelDamageAdd",damage_add_fn);
    settings=static_cast<const KilnSettingsApi*>(g_api->service(kstr(KILN_SETTINGS_SERVICE),1));
    if(!settings || settings->header.size<offsetof(KilnSettingsApi,get)+sizeof(void*) || !settings->get || !settings->find)settings=nullptr;
    // Mark session rows before mods load, including when their engine prerequisites are missing.
    const auto* caps=static_cast<const KilnCapsApi*>(g_api->service(kstr(KILN_CAPS_SERVICE),1));
    if(caps && caps->header.size>=offsetof(KilnCapsApi,report)+sizeof(void*) && caps->report)
        for(const char* name:{"build.block_info","build.find_wall","build.break_building","build.break_damage"})
            caps->report(g_owner,kstr(name),KILN_CAP_PENDING,KILN_CAP_REASON_SESSION,kstr("waiting for the server world"));
    block_caps();
}
void block_caps() {
    report("build.block_info",F.read,"build.read is unavailable",true);
    report("build.find_wall",F.read,"build.read is unavailable",true);
    report("build.block_metadata",metadata_on,"qualified building/terrain material lookup or layout missing",false);
    report("build.block_provenance",provenance_on,"qualified baked-world cell reader missing",false);
    report("build.break_building",F.read && F.write && metadata_on,"qualified building metadata or writer missing",true);
    report("build.break_damage",damage_on && settings,"qualified damage/crack-state writer or kiln.settings missing",true);
    report("build.break_drops",false,"qualified material drop/spawn path missing",false);
    report("build.break_terrain",F.read && F.write && metadata_on && provenance_on,"qualified provenance, metadata or guarded writer missing",false);
}
KilnResult KILN_CALL block_at(KilnVec3 p,KilnBuildBlockInfo* out) {
    if(!out || out->size<sizeof *out)return KILN_ERR_INVALID_ARGUMENT;
    try {
        blocks::Source src;Context c;uint32_t why=0;auto rc=prepare(src,c,why);
        return rc==KILN_OK?blocks::block_at(src,p,out):refused(out,rc,why);
    }catch(...){return refused(out,KILN_ERR_INTERNAL,KILN_BUILD_BLOCK_FAULTED);}
}
KilnResult KILN_CALL find_wall(KilnVec3 from,KilnVec3 to,uint32_t flags,KilnBuildBlockInfo* out) {
    if(!out || out->size<sizeof *out)return KILN_ERR_INVALID_ARGUMENT;
    try {
        blocks::Source src;Context c;uint32_t why=0;auto rc=prepare(src,c,why);
        return rc==KILN_OK?blocks::find_wall(src,from,to,flags,out):refused(out,rc,why);
    }catch(...){return refused(out,KILN_ERR_INTERNAL,KILN_BUILD_BLOCK_FAULTED);}
}
KilnResult KILN_CALL break_box(const KilnBuildBreak* q,KilnBuildBreakResult* out) {
    if(!out || out->size<sizeof *out)return KILN_ERR_INVALID_ARGUMENT;
    try {
        blocks::Source src;Context c;uint32_t why=0;auto rc=prepare(src,c,why);
        return rc==KILN_OK?blocks::break_box(src,q,out):refused(out,rc,why);
    }catch(...){
        // Allocation/readback may throw after a writer committed. Never assert that an exception rolled it back.
        auto rc=refused(out,KILN_ERR_INTERNAL,KILN_BUILD_BLOCK_FAULTED);
        out->flags=KILN_BUILD_BREAK_R_MAY_HAVE_CHANGED;return rc;
    }
}
} // namespace kb
