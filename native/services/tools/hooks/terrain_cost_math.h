// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace kiln_tools_payment {
// The engine supplies float voxel volume, not a raw fill counter. Promote before dividing, then allow a few float
// ULPs at the quotient's magnitude. A fixed tolerance alone overcharges larger exact multiples after float rounding.
// A positive volume always costs at least one item.
inline bool price(float volume, uint32_t voxels_per_item, uint32_t& count) {
    if (!std::isfinite(volume) || volume < 0 || !voxels_per_item) return false;
    if (volume == 0) { count = 0; return true; }
    const double quotient = static_cast<double>(volume) / voxels_per_item;
    const double tolerance = std::max(1e-6, std::abs(quotient) * 4 * std::numeric_limits<float>::epsilon());
    const double rounded = std::max(1.0, std::ceil(quotient - tolerance));
    if (rounded > std::numeric_limits<int32_t>::max()) return false;
    count = static_cast<uint32_t>(rounded);
    return true;
}
}
