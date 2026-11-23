// SPDX-License-Identifier: MIT
#include "../block_ops.h"
#include "../terrain_damage.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <tuple>
using namespace kb::blocks;
namespace {
using Key = std::tuple<int32_t,int32_t,int32_t>;
std::map<Key,KilnVoxel> cells;
int writes=0, reads=0,damage_calls=0; bool loaded=true, fail_write=false, unknown=false, unbreakable=false,deny=false,hard_health=false;
bool writer_verified=true,corrupt_unselected=false,bounded_reads=false;
std::map<Key,uint8_t> damage;
bool get_damage(void*,const int32_t* c,uint8_t,uint8_t* out){*out=damage[{c[0],c[1],c[2]}];return true;}
uint32_t add_damage(void*,const int32_t* c,uint8_t,uint8_t increment,double,uint8_t* out){++damage_calls;auto& d=damage[{c[0],c[1],c[2]}];d=static_cast<uint8_t>(std::min(255,static_cast<int>(d)+increment));*out=d;return KILN_BUILD_BLOCK_NONE;}
KilnResult read(void*,const int32_t* o,const uint32_t* d,KilnVoxel* out,size_t) {
    ++reads; if(!loaded) return KILN_ERR_UNAVAILABLE;
    if(bounded_reads)for(int a=0;a<3;++a)if(o[a]<0 || int64_t(o[a])+d[a]>16)return KILN_ERR_INVALID_ARGUMENT;
    size_t i=0; for(uint32_t z=0;z<d[2];++z) for(uint32_t y=0;y<d[1];++y) for(uint32_t x=0;x<d[0];++x)
        out[i++]=cells[{o[0]+x,o[1]+y,o[2]+z}];
    return KILN_OK;
}
bool facts(void*,uint8_t material, int32_t* hardness,int32_t* health) { *hardness=unbreakable?5:2; *health=hard_health && material==144?100000:100; return true; }
int32_t provenance(void*,const int32_t* o,KilnVoxel) { return unknown?-1:(o[0]==3?1:0); }
KilnResult write(void*,const KilnBuildWrite* w,KilnBuildResult* r) {
    ++writes;if(deny){r->flags=KILN_BUILD_R_ZONE;return KILN_ERR_REVOKED;}
    if(w->flags&KILN_BUILD_F_DRY_RUN){r->flags=KILN_BUILD_R_DRY_RUN;return KILN_OK;}
    size_t i=0; for(uint32_t z=0;z<w->dims[2];++z) for(uint32_t y=0;y<w->dims[1];++y) for(uint32_t x=0;x<w->dims[0];++x,++i)
        if(w->mask[i] && (!fail_write || x==0)) cells[{w->origin[0]+x,w->origin[1]+y,w->origin[2]+z}]=w->cells[i];
    if(corrupt_unselected)cells[{w->origin[0]+1,w->origin[1],w->origin[2]}]=0;
    r->flags=KILN_BUILD_R_CHANGED|(writer_verified?KILN_BUILD_R_VERIFIED:0); r->blocks_written=1;
    return fail_write?KILN_ERR_INTERNAL:KILN_OK;
}
Source source() { Source s{}; s.cell_size=.5; s.read=&read; s.write=&write; s.metadata=&facts; s.provenance=&provenance; if(bounded_reads)for(auto& max:s.maximum)max=16; return s; }
int total=0,fail=0;
void check(bool b,const char* name) { ++total;if(!b){++fail;std::fprintf(stderr,"FAIL %s\n",name);} }
KilnBuildBreak request() { KilnBuildBreak q{};q.size=sizeof q;q.flags=KILN_BUILD_BREAK_INSTANT;q.dims[0]=4;q.dims[1]=q.dims[2]=1;return q; }
}
int main() {
    Source s=source(); KilnBuildBlockInfo b{};b.size=sizeof b;
    cells[{0,0,0}]=KILN_VOXEL_MAKE(143,0); // building paint zero is not air
    check(block_at(s,{.1,.1,.1},&b)==KILN_OK && b.kind==KILN_BUILD_BLOCK_BUILDING,"unpainted building is solid");
    check(b.material==143 && b.fill==0 && b.hardness==2 && b.health==100 && b.damage==-1,"raw material paint and metadata");
    check(b.center.x==.25 && b.provenance==0,"cell center and player provenance");
    cells[{0,0,0}]=KILN_VOXEL_MAKE(2,0);
    check(block_at(s,{.1,.1,.1},&b)==KILN_OK && b.kind==KILN_BUILD_BLOCK_AIR,"terrain material without fill is air");
    cells[{-1,0,0}]=KILN_VOXEL_MAKE(2,1);
    check(block_at(s,{-.01,.1,.1},&b)==KILN_OK && b.center.x==-.25,"negative coordinates floor");
    check(block_at(s,{NAN,0,0},&b)==KILN_ERR_INVALID_ARGUMENT,"NaN rejected");
    check(block_at(s,{1e100,0,0},&b)==KILN_ERR_INVALID_ARGUMENT,"overflow rejected");
    loaded=false; check(block_at(s,{0,0,0},&b)==KILN_ERR_UNAVAILABLE && b.refusal==KILN_BUILD_BLOCK_NOT_LOADED,"unloaded read refuses"); loaded=true;
    cells.clear();cells[{1,0,0}]=KILN_VOXEL_MAKE(2,255);cells[{2,0,0}]=KILN_VOXEL_MAKE(143,0);
    check(find_wall(s,{.1,.1,.1},{3,.1,.1},0,&b)==KILN_OK && b.center.x==1.25,"default ray skips terrain");
    check(find_wall(s,{.1,.1,.1},{3,.1,.1},KILN_BUILD_FIND_PLAYER_TERRAIN,&b)==KILN_OK && b.center.x==.75,"player terrain ray");
    unknown=true;check(find_wall(s,{.1,.1,.1},{3,.1,.1},KILN_BUILD_FIND_PLAYER_TERRAIN,&b)==KILN_OK && b.center.x==1.25,"unknown terrain protected and skipped");unknown=false;
    check(find_wall(s,{.1,.1,.1},{3,.1,.1},KILN_BUILD_FIND_TERRAIN,&b)==KILN_OK && b.kind==KILN_BUILD_BLOCK_TERRAIN,"explicit all terrain ray");
    check(find_wall(s,{0,1,0},{100,1,0},0,&b)==KILN_ERR_NOT_FOUND,"clear line returns not found");
    cells[{129,2,0}]=KILN_VOXEL_MAKE(143,0);
    check(find_wall(s,{0,1,0},{100,1,0},0,&b)==KILN_ERR_NOT_FOUND,"ray clips at 64m");
    check(find_wall(s,{1.1,.1,.1},{1.1,.1,.1},KILN_BUILD_FIND_TERRAIN,&b)==KILN_OK,"zero ray checks starting cell");
    check(find_wall(s,{0,0,0},{1,0,0},4,&b)==KILN_ERR_INVALID_ARGUMENT,"unknown ray flags rejected");
    cells.clear();cells[{-1,0,0}]=KILN_VOXEL_MAKE(143,0);cells[{0,1,0}]=KILN_VOXEL_MAKE(144,0);
    check(find_wall(s,{.25,.25,.25},{0,.5,.25},0,&b)==KILN_OK && b.material==144 && b.center.x==.25 && b.center.y==.75,
        "mixed-direction endpoint tie never steps outside the endpoint cell");
    cells.clear();cells[{15,0,0}]=KILN_VOXEL_MAKE(143,0);
    check(find_wall(s,{.01,.1,.1},{7.5,.1,.1},0,&b)==KILN_OK && b.center.x==7.75,
        "endpoint cell is visited despite accumulated DDA roundoff");
    cells.clear();cells[{1,0,0}]=KILN_VOXEL_MAKE(143,0);cells[{0,1,0}]=KILN_VOXEL_MAKE(144,0);
    check(find_wall(s,{.25,.25,.25},{.75,.75,.25},0,&b)==KILN_OK && b.material==143,"interior DDA ties retain x before y order");
    auto q=request();KilnBuildBreakResult r{};r.size=sizeof r;
    cells.clear();for(int x=0;x<4;++x)cells[{x,0,0}]=KILN_VOXEL_MAKE(143,0);
    check(break_box(s,&q,&r)==KILN_OK && r.removed==3 && r.protected_cells==1 && writes==1,"instant breaks player blocks only");
    check(cells[{3,0,0}]==KILN_VOXEL_MAKE(143,0),"natural building remains");
    cells[{0,0,0}]=KILN_VOXEL_MAKE(143,0);unknown=true;int before=writes;
    check(break_box(s,&q,&r)==KILN_OK && r.unknown_cells==2 && writes==before,"unknown provenance never removed");unknown=false;
    q.flags|=KILN_BUILD_BREAK_WORLD;
    check(break_box(s,&q,&r)==KILN_OK && r.removed==2,"explicit natural building break");
    cells[{0,0,0}]=KILN_VOXEL_MAKE(143,0);unbreakable=true;before=writes;
    check(break_box(s,&q,&r)==KILN_OK && r.unbreakable_cells==1 && writes==before,"instant never breaks Unbreakable");unbreakable=false;
    q.flags|=KILN_BUILD_BREAK_DROPS;
    check(break_box(s,&q,&r)==KILN_ERR_UNAVAILABLE && r.refusal==KILN_BUILD_BLOCK_SYMBOL_MISSING && writes==before,"drops refuse all without qualified seam");q.flags&=~KILN_BUILD_BREAK_DROPS;
    q.flags&=~KILN_BUILD_BREAK_INSTANT;
    check(break_box(s,&q,&r)==KILN_ERR_UNAVAILABLE && writes==before,"damage refuses all without qualified seam");q.flags|=KILN_BUILD_BREAK_INSTANT;
    q.flags|=KILN_BUILD_BREAK_TERRAIN;s.provenance=nullptr;
    check(break_box(s,&q,&r)==KILN_ERR_UNAVAILABLE && writes==before,"terrain refuses all without provenance");s.provenance=&provenance;
    cells[{0,0,0}]=cells[{3,0,0}]=KILN_VOXEL_MAKE(2,255);
    check(break_box(s,&q,&r)==KILN_OK && r.removed==1 && r.protected_cells==1,"qualified exact terrain clear spares natural ground even with WORLD");q.flags&=~KILN_BUILD_BREAK_TERRAIN;
    cells[{0,0,0}]=KILN_VOXEL_MAKE(143,0);cells.erase({3,0,0});before=writes;
    s.metadata=nullptr;
    check(break_box(s,&q,&r)==KILN_ERR_UNAVAILABLE && writes==before,"missing metadata never bypasses Unbreakable");s.metadata=&facts;
    s.provenance=nullptr;q.flags&=~KILN_BUILD_BREAK_WORLD;
    check(break_box(s,&q,&r)==KILN_ERR_UNAVAILABLE && writes==before,"missing provenance refuses default break");s.provenance=&provenance;
    q.flags|=KILN_BUILD_BREAK_WORLD;uint8_t mask[4]={0,1,0,0};q.selection=mask;
    cells[{1,0,0}]=KILN_VOXEL_MAKE(144,0);
    check(break_box(s,&q,&r)==KILN_OK && r.removed==1 && cells[{0,0,0}]!=0,"selection mask keeps other cells");q.selection=nullptr;
    q.materials[143/8]=1u<<(143%8);cells[{1,0,0}]=KILN_VOXEL_MAKE(144,0);
    check(break_box(s,&q,&r)==KILN_OK && r.removed==1 && r.excluded_cells==1,"raw material filter");q.materials[143/8]=0;
    cells[{0,0,0}]=cells[{1,0,0}]=KILN_VOXEL_MAKE(143,0);fail_write=true;
    check(break_box(s,&q,&r)==KILN_ERR_INTERNAL && r.removed==1 && (r.flags&KILN_BUILD_BREAK_R_MAY_HAVE_CHANGED),"partial writer fault reports actual readback and may-have-changed");fail_write=false;
    q=request();q.dims[0]=1;q.flags=0;q.damage[2]=50;cells.clear();cells[{0,0,0}]=KILN_VOXEL_MAKE(143,0);
    s.damage_get=&get_damage;s.damage_add=&add_damage;s.mining_factor=1;
    check(break_box(s,&q,&r)==KILN_OK && r.damaged==1 && r.removed==0 && damage[{0,0,0}]==127,"half-health hit accumulates native damage");
    check(break_box(s,&q,&r)==KILN_OK && r.removed==0 && damage[{0,0,0}]==254,"damage floors each hit");
    check(break_box(s,&q,&r)==KILN_OK && r.removed==1 && r.damaged==1,"255 damage clears through guarded writer");
    cells[{0,0,0}]=KILN_VOXEL_MAKE(143,0);damage.clear();deny=true;int hits=damage_calls;
    check(break_box(s,&q,&r)==KILN_ERR_REVOKED && r.refusal==KILN_BUILD_BLOCK_ZONE && damage_calls==hits,"policy preflight before every damage mutation");deny=false;
    q.build_flags=KILN_BUILD_F_DRY_RUN;
    check(break_box(s,&q,&r)==KILN_OK && damage_calls==hits && !(r.flags&KILN_BUILD_BREAK_R_MAY_HAVE_CHANGED),"dryrun never damages");q.build_flags=0;
    cells[{0,0,0}]=KILN_VOXEL_MAKE(2,255);q.flags=KILN_BUILD_BREAK_TERRAIN;q.terrain_health_scale=250;
    check(break_box(s,&q,&r)==KILN_OK && r.damaged==0 && damage_calls==hits,"caller terrain health scale changes threshold");
    q.terrain_health_scale=1;q.damage[2]=100;
    check(break_box(s,&q,&r)==KILN_OK && r.removed==1,"generic terrain damage uses caller threshold");
    s.mining_factor=0;cells[{0,0,0}]=KILN_VOXEL_MAKE(143,0);q.flags=0;
    check(break_box(s,&q,&r)==KILN_ERR_UNAVAILABLE,"missing mining scale refuses damage");s.mining_factor=1;
    q=request();q.dims[0]=4097;before=writes;
    check(break_box(s,&q,&r)==KILN_ERR_INVALID_ARGUMENT && writes==before,"bounded box");q.dims[0]=4;
    q.damage[0]=-1;
    check(break_box(s,&q,&r)==KILN_ERR_INVALID_ARGUMENT,"negative damage rejected");q.damage[0]=0;
    q.flags|=128;
    check(break_box(s,&q,&r)==KILN_ERR_INVALID_ARGUMENT,"unknown break flags rejected");q.flags&=~128u;
    q.build_flags=KILN_BUILD_F_UNLOADED;
    check(break_box(s,&q,&r)==KILN_ERR_INVALID_ARGUMENT,"unloaded breaking forbidden");
    s=source();q=request();q.dims[0]=5;q.flags|=KILN_BUILD_BREAK_PROTECTED_SPHERE;
    q.sphere_center={.25,.25,.25};q.sphere_radius=2;q.sphere_margin=.01;q.sphere_minimum=.25;
    cells.clear();cells[{0,0,0}]=cells[{3,0,0}]=cells[{4,0,0}]=KILN_VOXEL_MAKE(143,0);
    check(break_box(s,&q,&r)==KILN_OK && r.removed==1 && cells[{4,0,0}]!=0,"protected building shrinks whole sphere and spares player block beyond it");
    cells[{0,0,0}]=KILN_VOXEL_MAKE(143,0);cells[{4,0,0}]=KILN_VOXEL_MAKE(2,255);q.flags|=KILN_BUILD_BREAK_TERRAIN;
    check(break_box(s,&q,&r)==KILN_OK && r.removed==2,"terrain keeps original sphere when building sphere shrinks");
    q.flags|=KILN_BUILD_BREAK_WORLD;cells[{4,0,0}]=KILN_VOXEL_MAKE(143,0);
    check(break_box(s,&q,&r)==KILN_OK && r.removed==2,"WORLD bypasses protected building shrink");
    q.flags=KILN_BUILD_BREAK_PROTECTED_SPHERE;q.damage[2]=1;hard_health=true;damage.clear();cells.clear();
    cells[{3,0,0}]=KILN_VOXEL_MAKE(144,0);cells[{4,0,0}]=KILN_VOXEL_MAKE(143,0);
    s.damage_get=&get_damage;s.damage_add=&add_damage;s.mining_factor=1;
    check(break_box(s,&q,&r)==KILN_OK && r.damaged==1 && damage[{4,0,0}]==2,"a protected material with zero damage increment does not shrink the sphere");hard_health=false;
    s=source();q=request();q.dims[0]=2;uint8_t one_cell[2]={1,0};q.selection=one_cell;
    cells.clear();cells[{0,0,0}]=cells[{1,0,0}]=KILN_VOXEL_MAKE(143,0);writer_verified=false;
    check(break_box(s,&q,&r)==KILN_ERR_INTERNAL && r.refusal==KILN_BUILD_BLOCK_UNVERIFIED && r.removed==1,
        "selected removals do not override failed full-block writer verification");writer_verified=true;
    cells[{0,0,0}]=KILN_VOXEL_MAKE(143,0);corrupt_unselected=true;
    check(break_box(s,&q,&r)==KILN_ERR_INTERNAL && r.refusal==KILN_BUILD_BLOCK_UNVERIFIED && r.removed==1,
        "readback detects collateral changes to unselected cells");corrupt_unselected=false;
    TerrainDamage terrain;const std::array<int32_t,3> at{1,2,3};uint8_t accumulated=0;
    check(terrain.add(100,at,2,127,20,accumulated) && accumulated==127,"terrain accumulator hit");
    check(terrain.get(100+19000000000LL,at,2)==127,"terrain damage before expiry");
    check(terrain.get(100+20000000000LL,at,2)==0,"terrain damage expires at caller duration");
    terrain.add(100,at,2,127,10,accumulated);
    check(terrain.get(200,at,3)==0,"material change forgets terrain health");
    terrain.add(100,at,2,255,10,accumulated);terrain.clear();
    check(terrain.get(101,at,2)==0,"world unload clears accumulated terrain health");
    check(!terrain.add(INT64_MAX,at,2,1,20,accumulated),"expiry overflow refuses");
    check(!terrain.add(INT64_MAX-20,at,2,1,1e-7,accumulated),"sub-microsecond expiry overflow refuses on Windows long double");
    terrain.clear();bool admitted=true;
    for(int x=0;x<65536;++x)admitted=terrain.add(100,{x,0,0},2,1,20,accumulated) && admitted;
    check(admitted,"terrain map admits its full documented capacity");
    check(!terrain.add(101,{-1,0,0},2,1,20,accumulated),"full live terrain map reports failure instead of silently evicting damage");
    // Exercise the real full accumulator through the production break loop, not only its helper.
    s=source();s.context=&terrain;s.mining_factor=1;
    s.damage_get=[](void* p,const int32_t* c,uint8_t m,uint8_t* out){*out=static_cast<TerrainDamage*>(p)->get(101,{c[0],c[1],c[2]},m);return true;};
    s.damage_add=[](void* p,const int32_t* c,uint8_t m,uint8_t n,double expiry,uint8_t* out)->uint32_t{bool full=false;return static_cast<TerrainDamage*>(p)->add(101,{c[0],c[1],c[2]},m,n,expiry,*out,&full)?KILN_BUILD_BLOCK_NONE:full?KILN_BUILD_BLOCK_DAMAGE_MAP_FULL:KILN_BUILD_BLOCK_FAULTED;};
    q=request();q.flags=KILN_BUILD_BREAK_TERRAIN;q.dims[0]=1;q.origin[0]=-1;q.damage[2]=1;
    cells.clear();cells[{-1,0,0}]=KILN_VOXEL_MAKE(2,255);before=writes;
    check(break_box(s,&q,&r)==KILN_ERR_UNAVAILABLE && r.refusal==24,"full terrain damage map is a capacity refusal, not a fault");
    check(!(r.flags&KILN_BUILD_BREAK_R_MAY_HAVE_CHANGED) && r.damaged==0 && writes==before+1 && cells[{-1,0,0}]!=0,
        "first-cell capacity refusal has no changes and only the policy dry run");
    q.origin[0]=0;cells[{0,0,0}]=KILN_VOXEL_MAKE(2,255);
    check(break_box(s,&q,&r)==KILN_OK && r.damaged==1,"capacity refusal does not disable later damage to tracked cells");
    check(terrain.add(101,{0,0,0},2,1,20,accumulated) && accumulated==4,"full terrain map still accumulates existing cells");
    q.origin[0]=65535;q.dims[0]=2;cells[{65535,0,0}]=cells[{65536,0,0}]=KILN_VOXEL_MAKE(2,255);
    check(break_box(s,&q,&r)==KILN_ERR_UNAVAILABLE && r.refusal==KILN_BUILD_BLOCK_DAMAGE_MAP_FULL && r.damaged==1 &&
        (r.flags&KILN_BUILD_BREAK_R_MAY_HAVE_CHANGED) && terrain.get(101,{65535,0,0},2)==3,
        "capacity refusal preserves earlier damage and honestly reports partial state");
    check(terrain.add(20000000101LL,{-1,0,0},2,1,20,accumulated) && accumulated==1,"expired terrain entries release map capacity");
    bounded_reads=true;s=source();
    for(int axis=0;axis<3;++axis)for(bool high:{false,true}) {
        q=request();q.flags|=KILN_BUILD_BREAK_PROTECTED_SPHERE;
        for(int a=0;a<3;++a){q.origin[a]=2;q.dims[a]=5;}
        q.origin[axis]=high?13:0;q.dims[axis]=3;
        double center[3]={2.25,2.25,2.25};center[axis]=high?7.75:.25;
        q.sphere_center={center[0],center[1],center[2]};q.sphere_radius=1;q.sphere_margin=.01;q.sphere_minimum=.25;
        int32_t target[3]={4,4,4};target[axis]=high?15:0;
        cells.clear();cells[{target[0],target[1],target[2]}]=KILN_VOXEL_MAKE(143,0);
        check(break_box(s,&q,&r)==KILN_OK && r.removed==1,"sphere protection scan clips to valid cells at every grid face");
    }
    bounded_reads=false;
    std::printf("block ops: %d passed, %d failed\n",total-fail,fail);return fail?1:0;
}
