// SPDX-License-Identifier: MIT
#pragma once
#include "services/kiln_build.h"

namespace kb::blocks {
// Pure block policy/geometry over a tick-owned source. No engine pointers escape this internal boundary.
struct Source {
    void* context = nullptr;
    double origin[3]{};
    double cell_size = 0;
    KilnResult (*read)(void*, const int32_t*, const uint32_t*, KilnVoxel*, size_t) = nullptr;
    KilnResult (*write)(void*, const KilnBuildWrite*, KilnBuildResult*) = nullptr;
    bool (*metadata)(void*, uint8_t, int32_t*, int32_t*) = nullptr;
    int32_t (*provenance)(void*, const int32_t*, KilnVoxel) = nullptr;
};
KilnResult block_at(const Source&, KilnVec3, KilnBuildBlockInfo*);
KilnResult find_wall(const Source&, KilnVec3, KilnVec3, uint32_t, KilnBuildBlockInfo*);
KilnResult break_box(const Source&, const KilnBuildBreak*, KilnBuildBreakResult*);
} // namespace kb::blocks
