// SPDX-License-Identifier: MIT
// The identical checked record walk used by the real and simulated game. Callers guard reads (SEH in game.cpp).
#pragma once
#include "core.h"
#include "resource_limits.h"
#include <limits>
namespace kvx {
constexpr uint64_t kMaxResourceRecords = 1u << 20;
struct ResourceTable { uintptr_t records = 0; uint64_t capacity = 0; };
template<class Table, class Read>
bool resource_pointer(const MapView& map, const uint8_t guid[16], size_t bytes, uint32_t stride, uint32_t offset,
                      Table table, Read read, uintptr_t* resource) {
    if (!bytes || bytes > kMaxVerifiedResourceBytes || stride < sizeof(uintptr_t) || offset > stride - sizeof(uintptr_t)) return false;
    const int64_t slot = map_slot(map, guid);
    if (slot < 0 || !map.values) return false;
    ResourceTable first, last;
    uint32_t index = 0;
    const uintptr_t values = reinterpret_cast<uintptr_t>(map.values);
    if (uint64_t(slot) > (std::numeric_limits<uintptr_t>::max()-values)/sizeof(uint32_t) ||
        !table(&first) || !first.records || !first.capacity || first.capacity > kMaxResourceRecords ||
        !read(values + uint64_t(slot)*sizeof(uint32_t), &index, sizeof index) || index >= first.capacity) return false;
    if (index > (std::numeric_limits<uintptr_t>::max() - first.records) / stride) return false;
    const uintptr_t record = first.records + uint64_t{index} * stride;
    if (record > std::numeric_limits<uintptr_t>::max() - offset) return false;
    if (!table(&last) || first.records != last.records || first.capacity != last.capacity || index >= last.capacity) return false;
    uintptr_t ptr = 0;
    if (!read(record + offset, &ptr, sizeof ptr) || !ptr || ptr > std::numeric_limits<uintptr_t>::max() - bytes ||
        !table(&last) || first.records != last.records || first.capacity != last.capacity || index >= last.capacity) return false;
    *resource = ptr;
    return true;
}
template<class Table, class Read>
bool resource_copy(const MapView& map, const uint8_t guid[16], void* out, size_t bytes, uint32_t stride, uint32_t offset,
                   Table table, Read read) {
    uintptr_t ptr = 0;
    return out && resource_pointer(map, guid, bytes, stride, offset, table, read, &ptr) && read(ptr, out, bytes);
}
} // namespace kvx
