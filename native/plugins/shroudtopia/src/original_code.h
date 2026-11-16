// SPDX-License-Identifier: MIT
// The shim's read-only decision. No Win32 dependencies; only an established function entry can qualify.
#pragma once
#include <array>
#include "../../../core/src/pe_text.h"

namespace kshroud::original {
// Both spans start at the unwind entry, and include every compared byte.
inline bool entry_range(const uint8_t* original, const uint8_t* live, size_t span, size_t offset, size_t count,
                        uintptr_t entry, uintptr_t base, size_t image_size, uintptr_t* destination) {
    if (offset > span || count > span - offset) return false;
    uintptr_t target = 0;
    const size_t width = kiln::pe::entry_jump(live, span, entry, base, image_size, &target);
    if (!width) return false;
    size_t patch_end = width;
    for (size_t pad = 0; pad < 20 && patch_end < span && live[patch_end] == 0x90; ++pad) ++patch_end;
    if (offset >= patch_end) return false;
    bool changed = false;
    for (size_t i = 0; i < span; ++i) {
        if (live[i] == original[i]) continue;
        if (i >= patch_end) return false;
        if (i >= offset && i - offset < count) changed = true;
    }
    if (!changed) return false;
    if (destination) *destination = target;
    return true;
}

struct View {
    uintptr_t base = 0;
    uint32_t image_size = 0, text_rva = 0;
    std::vector<uint8_t> bytes;  // file .text, relocated for the loaded exe

    template<class Copy>
    const uint8_t* range(uintptr_t address, size_t count, uintptr_t begin, uintptr_t end,
                         Copy copy, uintptr_t* destination) const {
        const uintptr_t text = base + text_rva;
        if (bytes.empty() || !count || count > 4096 || address < text || address - text >= bytes.size() ||
            count > bytes.size() - (address - text) || begin < text || begin > address || end <= address ||
            count > end - address || end - text > bytes.size() || address - begin >= 34) return nullptr;
        // Includes the whole jump even when the caller compares only an operand of it. The NOP tail is bounded
        // exactly as pe_text.h bounds hook padding. An unreadable byte or an unrelated patch keeps live memcmp.
        const size_t offset = address - begin;
        const size_t span = std::min<size_t>(end - begin, std::max<size_t>(34, offset + count));
        std::array<uint8_t, 4096 + 34> live{};
        if (!copy(live.data(), reinterpret_cast<const void*>(begin), span)) return nullptr;
        if (!entry_range(bytes.data() + (begin - text), live.data(), span, offset, count,
                         begin, base, image_size, destination)) return nullptr;
        return bytes.data() + (address - text);
    }
};
} // namespace kshroud::original
