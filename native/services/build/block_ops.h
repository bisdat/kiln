// SPDX-License-Identifier: MIT
#pragma once
#include "services/kiln_build.h"

namespace kb::blocks {
// Pure block policy/geometry over a tick-owned source. No engine pointers escape this internal boundary.
struct Source {
    void* context = nullptr;
    double origin[3]{};
    double cell_size = 0;
    uint64_t maximum[3]{}; // optional exclusive grid bounds (minimum zero); clip sphere protection reads only
    KilnResult (*read)(void*, const int32_t*, const uint32_t*, KilnVoxel*, size_t) = nullptr;
    KilnResult (*write)(void*, const KilnBuildWrite*, KilnBuildResult*) = nullptr;
    bool (*metadata)(void*, uint8_t, int32_t*, int32_t*) = nullptr;
    int32_t (*provenance)(void*, const int32_t*, KilnVoxel) = nullptr;
    bool (*damage_get)(void*, const int32_t*, uint8_t material, uint8_t*) = nullptr;
    // KILN_BUILD_BLOCK_NONE, DAMAGE_MAP_FULL (no mutation), or FAULTED (mutation may have started).
    uint32_t (*damage_add)(void*, const int32_t*, uint8_t material, uint8_t increment, double expiry_seconds, uint8_t*) = nullptr;
    void (*damage_forget)(void*, const int32_t*, uint8_t material) = nullptr;
    double mining_factor = 0; // 0 means unknown: never substitute a guessed world setting
};
KilnResult block_at(const Source&, KilnVec3, KilnBuildBlockInfo*);
KilnResult find_wall(const Source&, KilnVec3, KilnVec3, uint32_t, KilnBuildBlockInfo*);
KilnResult break_box(const Source&, const KilnBuildBreak*, KilnBuildBreakResult*);
} // namespace kb::blocks
