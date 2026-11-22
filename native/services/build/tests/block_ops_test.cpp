// SPDX-License-Identifier: MIT
#include "../block_ops.h"
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
int writes=0, reads=0; bool loaded=true, fail_write=false, unknown=false, unbreakable=false;
KilnResult read(void*,const int32_t* o,const uint32_t* d,KilnVoxel* out,size_t) {
    ++reads; if(!loaded) return KILN_ERR_UNAVAILABLE;
    size_t i=0; for(uint32_t z=0;z<d[2];++z) for(uint32_t y=0;y<d[1];++y) for(uint32_t x=0;x<d[0];++x)
        out[i++]=cells[{o[0]+x,o[1]+y,o[2]+z}];
    return KILN_OK;
}
bool facts(void*,uint8_t, int32_t* hardness,int32_t* health) { *hardness=unbreakable?5:2; *health=100; return true; }
int32_t provenance(void*,const int32_t* o,KilnVoxel) { return unknown?-1:(o[0]==3?1:0); }
KilnResult write(void*,const KilnBuildWrite* w,KilnBuildResult* r) {
    ++writes; size_t i=0; for(uint32_t z=0;z<w->dims[2];++z) for(uint32_t y=0;y<w->dims[1];++y) for(uint32_t x=0;x<w->dims[0];++x,++i)
        if(w->mask[i] && (!fail_write || x==0)) cells[{w->origin[0]+x,w->origin[1]+y,w->origin[2]+z}]=w->cells[i];
    r->flags=KILN_BUILD_R_CHANGED|KILN_BUILD_R_VERIFIED; r->blocks_written=1;
    return fail_write?KILN_ERR_INTERNAL:KILN_OK;
}
Source source() { Source s{}; s.cell_size=.5; s.read=&read; s.write=&write; s.metadata=&facts; s.provenance=&provenance; return s; }
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
    q.dims[0]=4097;before=writes;
    check(break_box(s,&q,&r)==KILN_ERR_INVALID_ARGUMENT && writes==before,"bounded box");q.dims[0]=4;
    q.damage[0]=-1;
    check(break_box(s,&q,&r)==KILN_ERR_INVALID_ARGUMENT,"negative damage rejected");q.damage[0]=0;
    q.flags|=128;
    check(break_box(s,&q,&r)==KILN_ERR_INVALID_ARGUMENT,"unknown break flags rejected");q.flags&=~128u;
    q.build_flags=KILN_BUILD_F_UNLOADED;
    check(break_box(s,&q,&r)==KILN_ERR_INVALID_ARGUMENT,"unloaded breaking forbidden");
    std::printf("block ops: %d passed, %d failed\n",total-fail,fail);return fail?1:0;
}
