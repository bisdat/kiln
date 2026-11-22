// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif
namespace kiln::tools {
// Exact client26430625 Cc3fe18 call-site layout. No borrowed frame pointer survives this call.
struct BrushFrame {uintptr_t stack,frame,output,exclude,ratio,include;};
enum class BrushFrameResult { Unchanged, Patched, WriteFault };
inline constexpr std::array<uint8_t,16> overgrowth_brush_tag{
    0xcf,0xa6,0xae,0x08,0xe8,0xe0,0x6c,0x4d,0x8a,0x29,0xa2,0x1f,0x46,0x93,0xe0,0x06};
inline BrushFrameResult patch_brush_frame(const BrushFrame& f,uint32_t root,const std::array<uint8_t,16>& tag) {
    // Native prologue: RBP=RSP+0x100. Accept only the Overgrowth branch's distinct owned temporaries.
    if(!root || !f.stack || f.stack>UINTPTR_MAX-0x2b28 || f.frame!=f.stack+0x100 ||
       f.output!=f.frame+0x690 || f.include!=f.stack+0x30 || f.ratio!=f.frame+0x100 || f.exclude!=f.stack+0x40)
        return BrushFrameResult::Unchanged;
    bool writing=false;
#ifdef _WIN32
    __try {
#endif
        uint32_t selected=0,resolved=0;uintptr_t info=0,data=0;uint64_t count=0,capacity=0;
        std::memcpy(&selected,reinterpret_cast<const void*>(f.frame+0x118),4);
        std::memcpy(&info,reinterpret_cast<const void*>(f.frame+0xe0),sizeof info);
        if(selected!=root || !info)return BrushFrameResult::Unchanged;
        std::memcpy(&resolved,reinterpret_cast<const void*>(info),4);
        std::memcpy(&data,reinterpret_cast<const void*>(f.output),sizeof data);
        std::memcpy(&count,reinterpret_cast<const void*>(f.output+8),8);
        std::memcpy(&capacity,reinterpret_cast<const void*>(f.output+16),8);
        if(resolved!=root || data!=f.frame+0x6b8 || count || capacity!=16 ||
           std::memcmp(reinterpret_cast<const void*>(f.include),overgrowth_brush_tag.data(),16) ||
           std::memcmp(reinterpret_cast<const void*>(f.ratio),overgrowth_brush_tag.data(),16))
            return BrushFrameResult::Unchanged;
        // Original tag values, registers, output and all persistent/callback arrays remain untouched until admission.
        // These two GUID buffers were populated by this native frame immediately before the call instruction.
        writing=true;
        std::memcpy(reinterpret_cast<void*>(f.include),tag.data(),16);
        std::memcpy(reinterpret_cast<void*>(f.ratio),tag.data(),16);
        return BrushFrameResult::Patched;
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // The adapter skips this one query if a write faulted; it must not execute a half-patched/unrestricted query.
        return writing?BrushFrameResult::WriteFault:BrushFrameResult::Unchanged;
    }
#else
    (void)writing;
#endif
}
} // namespace kiln::tools
