// SPDX-License-Identifier: MIT
#pragma once
#include "services/kiln_hooks.h"
#include <cstring>

namespace kitems {
// Both approved builds use this entry for the equipment-selection helper.
inline constexpr uint8_t kSelectEquipmentCode[] = {
    0x48,0x89,0x6c,0x24,0x18,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,
    0xec,0x30,0x4c,0x8b,0xa4,0x24,0x98,0x00,0x00,0x00,0x4c,0x8d,0xba,0x98,0x00,0x00,0x00
};
// Resolved symbols are file identities, not proof of live-code ownership. Keep registry-owned detours compatible,
// but fail closed on a foreign patch, unreadable bytes or a hooks table too old to qualify them.
template<size_t N>
bool qualified_code(const KilnHooksApi* hooks, uintptr_t address, const uint8_t (&expected)[N]) {
    if (!hooks || hooks->header.size < offsetof(KilnHooksApi, code_state) + sizeof(hooks->code_state) ||
        !hooks->read_original || !hooks->code_state || !address) return false;
    uint8_t original[N]{};
    uint32_t state = KILN_CODE_UNKNOWN;
    uint64_t generation = 0;
    return hooks->read_original(address, original, N) == KILN_OK && std::memcmp(original, expected, N) == 0 &&
        hooks->code_state(address, N, &state, &generation) == KILN_OK &&
        (state == KILN_CODE_ORIGINAL || state == KILN_CODE_MANAGED);
}
} // namespace kitems
