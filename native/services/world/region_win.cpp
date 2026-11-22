// SPDX-License-Identifier: MIT
// The memory-range check of kiln.world's named services: a range is committed, private, read-write memory, page by
// page. Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer), modules/game-runtime/native/named-services/
// read_and_update_named_service.cpp:13-24 (writable). VirtualQuery only: no allocation, no user-mode lock, no wait, so
// it is called inside a hold of the engine's gate as ShroudForge calls it inside its own.
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace kw {

bool region_writable(std::uintptr_t address, std::size_t size) {
    if (!address || address > UINTPTR_MAX - size) return false;
    const std::uintptr_t end = address + size;
    for (int regions = 0; address < end; ++regions) {
        if (regions >= 64) return false;  // a 64 KiB service never spans this many regions
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || info.Type != MEM_PRIVATE || info.Protect != PAGE_READWRITE)
            return false;
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (base > UINTPTR_MAX - info.RegionSize || base + info.RegionSize <= address) return false;
        address = (std::min)(end, base + info.RegionSize);
    }
    return true;
}

}  // namespace kw
