// SPDX-License-Identifier: MIT
#pragma once
#include "services/kiln_hooks.h"
#include <cstring>

namespace kt {
// A resolved symbol is not proof that its live entry still belongs to the qualified executable. Registry-owned
// detours are allowed, including after revoke; unknown or foreign edits must never grant a tool's authority.
template<size_t N>
bool qualified_entry(const KilnHooksApi* hooks, uintptr_t address, const uint8_t (&expected)[N]) {
    if (!hooks || hooks->header.size < offsetof(KilnHooksApi, code_state) + sizeof(hooks->code_state) ||
        !hooks->read_original || !hooks->code_state || !address) return false;
    uint8_t original[N]{};
    uint32_t state = KILN_CODE_UNKNOWN;
    uint64_t generation = 0;
    return hooks->read_original(address, original, N) == KILN_OK &&
           std::memcmp(original, expected, N) == 0 &&
           hooks->code_state(address, N, &state, &generation) == KILN_OK &&
           (state == KILN_CODE_ORIGINAL || state == KILN_CODE_MANAGED);
}
} // namespace kt
