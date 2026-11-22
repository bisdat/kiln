// SPDX-License-Identifier: MIT
// Generic block semantics informed by EMF v44-v47 (MIT, matenm); engine facts are separately qualified by Kiln.
#include "block_ops.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace kb::blocks {
namespace {
uint32_t kind(KilnVoxel v) {
    const unsigned m = KILN_VOXEL_MATERIAL(v);
    return m >= 128 ? KILN_BUILD_BLOCK_BUILDING : m && KILN_VOXEL_FILL(v) ? KILN_BUILD_BLOCK_TERRAIN : KILN_BUILD_BLOCK_AIR;
}
bool cell(const Source& s, KilnVec3 p, int32_t out[3]) {
    const double v[3] = {p.x,p.y,p.z};
    if (!std::isfinite(s.cell_size) || s.cell_size <= 0) return false;
    for (int a=0;a<3;++a) {
        const double c=std::floor((v[a]-s.origin[a])/s.cell_size);
        // Leave a margin for DDA stepping and box arithmetic, before any floating-to-integer conversion.
        if (!std::isfinite(c) || c < INT32_MIN+8192.0 || c > INT32_MAX-8192.0) return false;
        out[a]=static_cast<int32_t>(c);
    }
    return true;
}
KilnBuildBlockInfo empty(uint32_t size) {
    KilnBuildBlockInfo b{}; b.size=size;b.provenance=-1;b.hardness=b.health=b.damage=-1;return b;
}
KilnResult read_info(const Source& s,const int32_t c[3],KilnBuildBlockInfo& b) {
    if (!s.read) { b.refusal=KILN_BUILD_BLOCK_SYMBOL_MISSING;return KILN_ERR_UNAVAILABLE; }
    KilnVoxel v=0;const uint32_t d[3]={1,1,1};
    const KilnResult rc=s.read(s.context,c,d,&v,1);
    if (rc!=KILN_OK) { b.refusal=rc==KILN_ERR_INTERNAL?KILN_BUILD_BLOCK_FAULTED:KILN_BUILD_BLOCK_NOT_LOADED;return rc; }
    b.kind=kind(v);b.material=KILN_VOXEL_MATERIAL(v);b.fill=KILN_VOXEL_FILL(v);
    b.center={s.origin[0]+(c[0]+.5)*s.cell_size,s.origin[1]+(c[1]+.5)*s.cell_size,s.origin[2]+(c[2]+.5)*s.cell_size};
    if (b.kind!=KILN_BUILD_BLOCK_AIR) {
        if (s.metadata) {
            int32_t h=-1,hp=-1;
            if(s.metadata(s.context,static_cast<uint8_t>(b.material),&h,&hp) && h>=0 && h<=5 && hp>=0) {
                b.hardness=h;b.health=hp;
            }
        }
        if(s.provenance) b.provenance=s.provenance(s.context,c,v);
    }
    return KILN_OK;
}
}
KilnResult block_at(const Source& s,KilnVec3 p,KilnBuildBlockInfo* out) {
    if(!out || out->size<sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    auto b=empty(out->size);int32_t c[3];
    if(!cell(s,p,c)) { b.refusal=KILN_BUILD_BLOCK_INVALID;*out=b;return KILN_ERR_INVALID_ARGUMENT; }
    const auto rc=read_info(s,c,b);*out=b;return rc;
}
KilnResult find_wall(const Source& s,KilnVec3 from,KilnVec3 to,uint32_t flags,KilnBuildBlockInfo* out) {
    if(!out || out->size<sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    auto b=empty(out->size);
    auto fail=[&](KilnResult rc,uint32_t why){b.refusal=why;*out=b;return rc;};
    int32_t c[3],end[3];
    if((flags&~3u) || !cell(s,from,c) || !cell(s,to,end)) return fail(KILN_ERR_INVALID_ARGUMENT,KILN_BUILD_BLOCK_INVALID);
    if((flags&KILN_BUILD_FIND_PLAYER_TERRAIN) && !(flags&KILN_BUILD_FIND_TERRAIN) && !s.provenance)
        return fail(KILN_ERR_UNAVAILABLE,KILN_BUILD_BLOCK_SYMBOL_MISSING);
    const double p[3]={from.x,from.y,from.z};double delta[3]={to.x-from.x,to.y-from.y,to.z-from.z};
    const double length=std::hypot(delta[0],delta[1],delta[2]);
    if(length>64) for(auto& d:delta)d*=64/length;
    KilnVec3 clipped{from.x+delta[0],from.y+delta[1],from.z+delta[2]};
    if(!cell(s,clipped,end))return fail(KILN_ERR_INVALID_ARGUMENT,KILN_BUILD_BLOCK_INVALID);
    double next[3],step_time[3];int step[3];
    for(int a=0;a<3;++a) {
        step[a]=delta[a]>0?1:delta[a]<0?-1:0;
        step_time[a]=step[a]?s.cell_size/std::abs(delta[a]):std::numeric_limits<double>::infinity();
        next[a]=step[a]?(s.origin[a]+(c[a]+(step[a]>0?1.0:0.0))*s.cell_size-p[a])/delta[a]:std::numeric_limits<double>::infinity();
    }
    // 64m on the qualified 0.5m grid crosses fewer than 400 cells. Bound synthetic/bad sources too.
    for(unsigned n=0;n<4096;++n) {
        b=empty(out->size);const auto rc=read_info(s,c,b);
        if(rc!=KILN_OK){*out=b;return rc;}
        if(b.kind==KILN_BUILD_BLOCK_BUILDING || (b.kind==KILN_BUILD_BLOCK_TERRAIN &&
           ((flags&KILN_BUILD_FIND_TERRAIN) || ((flags&KILN_BUILD_FIND_PLAYER_TERRAIN) && b.provenance==0)))) {
            *out=b;return KILN_OK;
        }
        if(c[0]==end[0] && c[1]==end[1] && c[2]==end[2]) { *out=empty(out->size);return KILN_ERR_NOT_FOUND; }
        // Deterministic x,y,z tie order, like EMF: visit the boundary cells rather than leaping diagonally.
        int a=next[0]<=next[1] && next[0]<=next[2]?0:next[1]<=next[2]?1:2;
        if(next[a]>1) { *out=empty(out->size);return KILN_ERR_NOT_FOUND; }
        c[a]+=step[a];next[a]+=step_time[a];
    }
    return fail(KILN_ERR_INVALID_ARGUMENT,KILN_BUILD_BLOCK_INVALID);
}
KilnResult break_box(const Source& s,const KilnBuildBreak* q,KilnBuildBreakResult* out) {
    if(!out || out->size<sizeof *out)return KILN_ERR_INVALID_ARGUMENT;
    KilnBuildBreakResult r{};r.size=out->size;r.write.size=sizeof r.write;
    auto fail=[&](KilnResult rc,uint32_t why){r.refusal=why;*out=r;return rc;};
    if(!q || q->size<sizeof *q || (q->flags&~15u) ||
       (q->build_flags&~(KILN_BUILD_F_RESPECT_ZONES|KILN_BUILD_F_IN_BASES|KILN_BUILD_F_DRY_RUN|KILN_BUILD_F_TRANSIENT)))
        return fail(KILN_ERR_INVALID_ARGUMENT,KILN_BUILD_BLOCK_INVALID);
    uint64_t count=1;
    for(int a=0;a<3;++a) {
        if(!q->dims[a] || q->dims[a]>4096 || q->origin[a]<INT32_MIN+8192 ||
           static_cast<int64_t>(q->origin[a])+q->dims[a]>INT32_MAX-8192)
            return fail(KILN_ERR_INVALID_ARGUMENT,KILN_BUILD_BLOCK_INVALID);
        count*=q->dims[a];
    }
    if(count>4096)return fail(KILN_ERR_INVALID_ARGUMENT,KILN_BUILD_BLOCK_INVALID);
    for(auto d:q->damage)if(d<0 || d>100000)return fail(KILN_ERR_INVALID_ARGUMENT,KILN_BUILD_BLOCK_INVALID);
    // Unqualified damage/crack, drop and exact terrain-clear seams are never simulated by raw writes.
    if(!(q->flags&KILN_BUILD_BREAK_INSTANT) || (q->flags&(KILN_BUILD_BREAK_DROPS|KILN_BUILD_BREAK_TERRAIN)) ||
       !s.metadata || !s.read || !s.write || (!(q->flags&KILN_BUILD_BREAK_WORLD) && !s.provenance))
        return fail(KILN_ERR_UNAVAILABLE,KILN_BUILD_BLOCK_SYMBOL_MISSING);
    std::vector<KilnVoxel> before(count),after(count),back(count);std::vector<uint8_t> mask(count,0);
    auto rc=s.read(s.context,q->origin,q->dims,before.data(),before.size());
    if(rc!=KILN_OK)return fail(rc,rc==KILN_ERR_INTERNAL?KILN_BUILD_BLOCK_FAULTED:KILN_BUILD_BLOCK_NOT_LOADED);
    after=before;bool filtered=false;for(auto m:q->materials)filtered|=m!=0;
    size_t i=0;uint64_t selected=0;
    for(uint32_t z=0;z<q->dims[2];++z)for(uint32_t y=0;y<q->dims[1];++y)for(uint32_t x=0;x<q->dims[0];++x,++i) {
        if((q->selection && !q->selection[i]) || kind(before[i])==KILN_BUILD_BLOCK_AIR)continue;
        ++r.considered;const uint8_t m=KILN_VOXEL_MATERIAL(before[i]);
        if(m<128 || (filtered && !(q->materials[m/8]&(1u<<(m%8))))) {++r.excluded_cells;continue;}
        int32_t h=-1,hp=-1;
        if(!s.metadata(s.context,m,&h,&hp) || h<0 || h>5 || hp<0){++r.unknown_cells;continue;}
        if(h==5){++r.unbreakable_cells;continue;}
        if(!(q->flags&KILN_BUILD_BREAK_WORLD)) {
            const int32_t c[3]={q->origin[0]+static_cast<int32_t>(x),q->origin[1]+static_cast<int32_t>(y),q->origin[2]+static_cast<int32_t>(z)};
            const auto p=s.provenance(s.context,c,before[i]);
            if(p!=0){if(p==1)++r.protected_cells;else ++r.unknown_cells;continue;}
        }
        mask[i]=1;after[i]=KILN_VOXEL_AIR;++selected;
    }
    if(!selected){r.flags=KILN_BUILD_BREAK_R_VERIFIED;*out=r;return KILN_OK;}
    KilnBuildWrite w{};w.size=sizeof w;w.flags=q->build_flags;w.cells=after.data();w.mask=mask.data();
    std::memcpy(w.origin,q->origin,sizeof w.origin);std::memcpy(w.dims,q->dims,sizeof w.dims);
    w.actor=q->actor;w.origin_mod=q->origin_mod;
    rc=s.write(s.context,&w,&r.write);
    if(r.write.flags&KILN_BUILD_R_DRY_RUN){*out=r;return rc;}
    if(rc==KILN_ERR_REVOKED) return fail(rc,(r.write.flags&KILN_BUILD_R_ZONE)?KILN_BUILD_BLOCK_ZONE:KILN_BUILD_BLOCK_BASE);
    if(rc==KILN_ERR_UNAVAILABLE)return fail(rc,(r.write.flags&KILN_BUILD_R_NOT_LOADED)?KILN_BUILD_BLOCK_NOT_LOADED:KILN_BUILD_BLOCK_NO_SESSION);
    if(rc!=KILN_OK && rc!=KILN_ERR_INTERNAL)return fail(rc,KILN_BUILD_BLOCK_INVALID);
    r.flags|=KILN_BUILD_BREAK_R_MAY_HAVE_CHANGED;
    // Count only what really became air, even after a fault. A writer's cells_changed may be merely planned.
    if(s.read(s.context,q->origin,q->dims,back.data(),back.size())==KILN_OK) {
        for(size_t j=0;j<count;++j)if(mask[j] && kind(back[j])==KILN_BUILD_BLOCK_AIR)++r.removed;
        r.flags|=KILN_BUILD_BREAK_R_VERIFIED;
    }
    if(rc!=KILN_OK)return fail(rc,KILN_BUILD_BLOCK_FAULTED);
    if(!(r.flags&KILN_BUILD_BREAK_R_VERIFIED) || r.removed!=selected)return fail(KILN_ERR_INTERNAL,KILN_BUILD_BLOCK_UNVERIFIED);
    r.damaged=r.removed;*out=r;return KILN_OK;
}
} // namespace kb::blocks
