// SPDX-License-Identifier: MIT
#include "../impact_args.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

struct Fixture {
    std::array<kab::ImpactVariable, 10> layout{};
    std::array<uint8_t, 256> data{};
    std::array<uint32_t, 9> handles{};
    kab::NativeCtx ctx{};
    kab::ArgSlice args{};
    Fixture() {
        uint32_t types[] = {0x737c8f86,0x9493a8c5,0x4835c9e9,0xc4b5300b,0x10e7bb7a,0xe89f7410,0xe89f7410,0xe89f7410,0x9859f31e,0x5bfc0dbb};
        uint16_t sizes[] = {4,56,1,20,4,4,4,4,4,4};
        uint16_t offsets[] = {0,16,80,96,128,144,160,176,192,208};
        for (size_t i=0;i<layout.size();++i) { layout[i].type=types[i]; layout[i].size=sizes[i]; layout[i].offset=offsets[i]; if(i<9) handles[i]=0xffff0000u|uint32_t(i); }
        layout[9].config_id=0x220e6b10;
        uint32_t actor=7,brush=42; float radius=2;
        std::memcpy(data.data(),&actor,4); std::memcpy(data.data()+208,&brush,4);
        data[80]=0xff; std::memcpy(data.data()+144,&radius,4); std::memcpy(data.data()+160,&radius,4);
        ctx.layout=reinterpret_cast<const uint8_t*>(layout.data()); ctx.layout_count=layout.size(); ctx.data=data.data(); ctx.data_size=data.size(); ctx.program_id=123;
        args={handles.data(),handles.size()};
    }
};
int main() {
    int checks=0;
    auto check=[&](bool b){assert(b);++checks;};
    Fixture f; kt::ImpactCopy c;
    check(kt::copy_tool_impact(f.ctx,f.args,KILN_TOOL_POLICY_REMOVE,c));
    check(c.actor==7 && c.used_item==42 && c.radius==2);
    check(c.context.data!=f.ctx.data && c.context.data[80]==0 && f.data[80]==0xff);
    int32_t damage=0; std::memcpy(&damage,c.context.data+96,4); check(damage==INT32_MAX);
    check(kt::copy_tool_impact(f.ctx,f.args,KILN_TOOL_POLICY_ADD,c) && c.context.data[80]==1);
    f.data[0]=0; check(!kt::copy_tool_impact(f.ctx,f.args,KILN_TOOL_POLICY_REMOVE,c)); f=Fixture{};
    f.ctx.layout=reinterpret_cast<const uint8_t*>(f.layout.data()); f.ctx.data=f.data.data(); f.args.data=f.handles.data();
    float nan=std::numeric_limits<float>::quiet_NaN(); std::memcpy(f.data.data()+144,&nan,4);
    check(!kt::copy_tool_impact(f.ctx,f.args,KILN_TOOL_POLICY_REMOVE,c));
    float r=9; std::memcpy(f.data.data()+144,&r,4); check(!kt::copy_tool_impact(f.ctx,f.args,KILN_TOOL_POLICY_REMOVE,c));
    r=2; std::memcpy(f.data.data()+144,&r,4);
    f.layout[2].size=4; check(!kt::copy_tool_impact(f.ctx,f.args,KILN_TOOL_POLICY_REMOVE,c)); f.layout[2].size=1;
    f.handles[3]=0; check(!kt::copy_tool_impact(f.ctx,f.args,KILN_TOOL_POLICY_REMOVE,c)); f.handles[3]=0xffff0003u;
    f.layout[1].offset=240; check(!kt::copy_tool_impact(f.ctx,f.args,KILN_TOOL_POLICY_REMOVE,c)); f.layout[1].offset=16;
    f.layout[9].config_id=0; check(!kt::copy_tool_impact(f.ctx,f.args,KILN_TOOL_POLICY_REMOVE,c)); f.layout[9].config_id=0x220e6b10;
    f.ctx.data_size=65537; check(!kt::copy_tool_impact(f.ctx,f.args,KILN_TOOL_POLICY_REMOVE,c)); f.ctx.data_size=256;
    check(!kt::copy_tool_impact(f.ctx,f.args,0,c));
    std::printf("%d impact argument checks passed\n",checks);
}
