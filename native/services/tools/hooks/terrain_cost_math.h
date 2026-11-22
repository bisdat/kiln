// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace kiln_tools_payment {
// The engine supplies float voxel volume, not a raw fill counter. Promote before dividing. A tolerance of one
// millionth of an item absorbs float noise at exact multiples; a positive volume always costs at least one item.
inline bool price(float volume, uint32_t voxels_per_item, uint32_t& count) {
    if (!std::isfinite(volume) || volume < 0 || !voxels_per_item) return false;
    if (volume == 0) { count = 0; return true; }
    const double rounded = std::max(1.0, std::ceil(static_cast<double>(volume) / voxels_per_item - 1e-6));
    if (rounded > std::numeric_limits<int32_t>::max()) return false;
    count = static_cast<uint32_t>(rounded);
    return true;
}
}
