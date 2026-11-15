// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
namespace kiln::hooks::chain {
inline bool checked_patch_allowed(size_t unchecked, size_t in_bytes) { return unchecked == 0 && in_bytes == 0; }
}
