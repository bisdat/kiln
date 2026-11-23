// SPDX-License-Identifier: MIT
#define main boundary_fixture
#include "presentation_api.cpp"
#undef main
#include "../brush_frame.h"
static uint32_t held_item;
static uint32_t KILN_CALL fixture_held() {return held_item;}
static KilnMidFn mid_callback;static void* mid_user;static uintptr_t mid_site;
static KilnResult KILN_CALL checked_mid(KilnOwner,uintptr_t target,const KilnHookOrder*,KilnMidFn fn,void* user,KilnHandle* out,uint32_t* unchecked) {
    mid_callback=fn;mid_user=user;mid_site=target;*out=55;*unchecked=0;return KILN_OK;
}
struct Frame {
    alignas(16) std::array<uint8_t,0x2b28> bytes{};
    uint32_t info{};
    KilnRegisters regs{};
    explicit Frame(uint32_t item) :info(item) {
        regs.rsp=reinterpret_cast<uintptr_t>(bytes.data());regs.rbp=regs.rsp+0x100;
        regs.rcx=regs.rbp+0x690;regs.rdx=regs.rsp+0x40;regs.r8=regs.rbp+0x100;regs.r9=regs.rsp+0x30;
        const uintptr_t ptr=reinterpret_cast<uintptr_t>(&info),data=regs.rbp+0x6b8;const uint64_t cap=16;
        std::memcpy(reinterpret_cast<void*>(regs.rbp+0x118),&item,4);std::memcpy(reinterpret_cast<void*>(regs.rbp+0xe0),&ptr,sizeof ptr);
        std::memcpy(reinterpret_cast<void*>(regs.rcx),&data,sizeof data);std::memcpy(reinterpret_cast<void*>(regs.rcx+16),&cap,8);
        std::memcpy(reinterpret_cast<void*>(regs.r9),overgrowth_brush_tag.data(),16);std::memcpy(reinterpret_cast<void*>(regs.r8),overgrowth_brush_tag.data(),16);
    }
    bool own_tag(const uint8_t* tag) const {return !std::memcmp(reinterpret_cast<const void*>(regs.r9),tag,16);}
};
static void invoke(uintptr_t site,Frame& frame) {if(site==mid_site)mid_callback(&frame.regs,mid_user);}
int main() {
    boundary_fixture();
    KilnApi fake{};fake.size=sizeof fake;fake.abi_version=KILN_ABI_VERSION;fake.service=&service;fake.subscribe_tick=&tick;fake.unsubscribe=&unsubscribe;fake.owner_revoke=&owner_revoke;
    api=&fake;hooks=&fake_hooks;child=88;retiring=99;
    for(uint32_t i=0;i<KILN_TOOL_HOOK_COUNT;++i)kiln_tools_report_hook(i,1);
    auto family=definition();KilnToolBrush brushes[]={{3155237504u,0,0.5},{3172015123u,0,1},{3255903218u,0,2}};family.brushes=brushes;family.brush_count=3;
    KilnHandle handle=0;CHECK(kiln_tools_define_family(7,&family,&handle)==KILN_OK);
    fake_hooks.mid2_checked=&checked_mid;
    brush_query_resume.store(0xc3fe1d);
    CHECK(brush_frame_hook.install_checked(&fake,child,0xc3fe18,&brush_query_frame,"brush fixture"));
    kiln_tools_presentation_set_held_provider(&fixture_held);held_item=family.tool;
    Frame own(family.tool);const auto saved=own.regs;invoke(0xc3fe18,own);
    CHECK(own.own_tag(family.identity));CHECK(!std::memcmp(reinterpret_cast<const void*>(own.regs.r8),family.identity,16));
    CHECK(!std::memcmp(&saved,&own.regs,sizeof saved)); // No register/stack redirection for a successful call.
    // Native tag matching admits all three family brushes and excludes the vanilla tagged brush.
    unsigned selected=0;for(const auto& brush:brushes){(void)brush;if(own.own_tag(family.identity))++selected;}
    CHECK(selected==3&&!own.own_tag(overgrowth_brush_tag.data()));
    Frame vanilla(0xbe8b0750);const auto vanilla_bytes=vanilla.bytes;held_item=0xbe8b0750;invoke(0xc3fe18,vanilla);CHECK(vanilla.bytes==vanilla_bytes);
    held_item=family.tool;
    for(auto other:{uintptr_t(0xd180a0),uintptr_t(0xd20190),uintptr_t(0xd6aa05)}) {Frame callback(family.tool);const auto before=callback.bytes;invoke(other,callback);CHECK(callback.bytes==before);}
    Frame stale(0xbe8b0750);const auto stale_bytes=stale.bytes;invoke(0xc3fe18,stale);CHECK(stale.bytes==stale_bytes);
    Frame changed(family.tool);changed.info=123;const auto changed_bytes=changed.bytes;invoke(0xc3fe18,changed);CHECK(changed.bytes==changed_bytes);
    Frame nested(family.tool);const auto nested_bytes=nested.bytes;held_item=0;invoke(0xc3fe18,nested);CHECK(nested.bytes==nested_bytes); // No inherited scope from outer own call.
    held_item=family.tool;Frame decay(family.tool);std::swap(decay.regs.r9,decay.regs.rdx);const auto decay_bytes=decay.bytes;invoke(0xc3fe18,decay);CHECK(decay.bytes==decay_bytes);
    Frame modified(family.tool);*reinterpret_cast<uint8_t*>(modified.regs.r9)=0;const auto modified_bytes=modified.bytes;invoke(0xc3fe18,modified);CHECK(modified.bytes==modified_bytes);
#ifdef _WIN32
    Frame fault(family.tool);auto* memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,fault.bytes.size(),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));CHECK(memory);
    std::memcpy(memory,fault.bytes.data(),fault.bytes.size());
    const auto delta=reinterpret_cast<uintptr_t>(memory)-fault.regs.rsp;
    fault.regs.rsp+=delta;fault.regs.rbp+=delta;fault.regs.rcx+=delta;fault.regs.rdx+=delta;fault.regs.r8+=delta;fault.regs.r9+=delta;
    const uintptr_t data=fault.regs.rbp+0x6b8;std::memcpy(reinterpret_cast<void*>(fault.regs.rcx),&data,sizeof data);
    DWORD old=0;CHECK(VirtualProtect(memory,4096,PAGE_READONLY,&old));invoke(0xc3fe18,fault);
    CHECK(fault.regs.rip==0xc3fe1d&&!kiln_tools_hook_installed(KILN_TOOL_HOOK_BRUSH_PICKER));
    CHECK(fault.own_tag(overgrowth_brush_tag.data()));CHECK(VirtualProtect(memory,4096,old,&old));CHECK(VirtualFree(memory,0,MEM_RELEASE));
#endif
    kiln_tools_presentation_uninstall();
    std::cout<<"brush frame: "<<checks<<" checks passed (includes boundary fixture)\n";
}
