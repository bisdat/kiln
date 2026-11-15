// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>

#include "services/kiln_hooks.h"

namespace kiln::hooks::chain {
inline bool checked_patch_allowed(size_t unchecked, size_t in_bytes) { return unchecked == 0 && in_bytes == 0; }
// What detour_typed_checked answers for an installation that came back `rc` with `unchecked` threads it could not
// check (or that stood in the bytes): a refusal for the threads is KILN_HOOKS_ERR_UNCHECKED, worth a later attempt; any
// other result is the installation's own.
inline KilnResult checked_install_result(KilnResult rc, uint32_t unchecked) {
    return rc != KILN_OK && unchecked != 0 ? static_cast<KilnResult>(KILN_HOOKS_ERR_UNCHECKED) : rc;
}
}
