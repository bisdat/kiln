// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
namespace kitems::storage {
struct ClientActionLayout {
    uint32_t offset = 0, version = 0, consumed = 0, size = 0;
    std::vector<uint32_t> refs;
};
// Each ref is the u32 entity itself or the first u32 of an InventorySlotId. Callers validate reflected ranges once.
template <class Contains>
bool forbidden_action(const uint8_t* payload, const ClientActionLayout& action, Contains contains) {
    for (auto off : action.refs) {
        if (off > action.size || action.size - off < 4)
            return true;
        uint32_t entity = 0;
        std::memcpy(&entity, payload + off, 4);
        if (entity && contains(entity))
            return true;
    }
    return false;
}
} // namespace kitems::storage
