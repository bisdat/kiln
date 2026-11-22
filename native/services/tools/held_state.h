// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
namespace kiln::tools {
// A reader copies complete component bytes into caller-owned storage. The production reader is used only
// during the client world's Post, never from an arbitrary selector/renderer hook.
struct HeldComponents {
    virtual ~HeldComponents()=default;
    virtual bool snapshot(uint32_t entity,uint32_t hash,void* out,size_t size)=0;
};
inline uint32_t held_u32(const uint8_t* bytes,size_t at) {uint32_t value;std::memcpy(&value,bytes+at,4);return value;}
inline uint32_t read_held_item(HeldComponents& source,uint32_t actor) {
    if(!actor)return 0;
    std::array<uint8_t,216> selection{},again{};
    if(!source.snapshot(actor,0x34bc3e2fu,selection.data(),selection.size()))return 0;
    const uint32_t action=held_u32(selection.data(),0xb0);
    if(action)return action; // Native MainHand resolver: the action's cached item id wins, not merely slotId.
    const uint8_t index=selection[0xc0];if(index>=16)return 0;
    const uint32_t inventory=held_u32(selection.data(),0xa0+4*(index>>3));if(!inventory)return 0;
    std::array<uint8_t,96> stacks{};
    if(!source.snapshot(inventory,0xb19528a9u,stacks.data(),stacks.size())||
       !source.snapshot(actor,0x34bc3e2fu,again.data(),again.size())||selection!=again)return 0;
    const size_t offset=12*(index&7);
    return held_u32(stacks.data(),offset+4)?held_u32(stacks.data(),offset):0;
}
} // namespace kiln::tools
