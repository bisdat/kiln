// SPDX-License-Identifier: MIT
// Bounded import walk over a mapped x64 PE. Reads and slot writes are supplied by the Win32 adapter or host test.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace kshroud::original {
template<class Read, class Write>
size_t patch_memcmp(uint8_t* image, size_t size, uintptr_t shim, Read copy, Write write) {
    auto read = [&](size_t at, void* out, size_t n) {
        return image && at <= size && n <= size - at && copy(out, image + at, n);
    };
    auto u16 = [&](size_t at, uint16_t& v) { return read(at, &v, 2); };
    auto u32 = [&](size_t at, uint32_t& v) { return read(at, &v, 4); };
    uint16_t mz = 0, machine = 0, magic = 0, opt_size = 0;
    uint32_t pe = 0, sig = 0, mapped = 0, dirs = 0, imports = 0, length = 0;
    if (!shim || !u16(0, mz) || mz != 0x5A4D || !u32(0x3C, pe) || pe < 0x40 ||
        !u32(pe, sig) || sig != 0x4550) return 0;
    const size_t opt = size_t(pe) + 24;
    if (!u16(size_t(pe) + 4, machine) || machine != 0x8664 || !u16(size_t(pe) + 20, opt_size) ||
        opt_size < 128 || opt > size || opt_size > size - opt || !u16(opt, magic) || magic != 0x20B ||
        !u32(opt + 56, mapped) || mapped != size || !u32(opt + 108, dirs) || dirs < 2 ||
        dirs > (opt_size - 112u) / 8u || !u32(opt + 120, imports) || !u32(opt + 124, length)) return 0;
    if (!imports || length < 20 || imports > size || length > size - imports) return 0;
    std::vector<size_t> slots;
    bool terminated = false;
    for (size_t at = imports; at <= size_t(imports) + length - 20; at += 20) {
        uint32_t descriptor[5]{};
        if (!read(at, descriptor, sizeof descriptor)) return 0;
        if (!(descriptor[0] | descriptor[1] | descriptor[2] | descriptor[3] | descriptor[4])) {
            terminated = true;
            break;
        }
        const size_t names = descriptor[0], iat = descriptor[4];
        if (!names) continue;  // resolved/bound IAT without import names: leave it alone
        if (!iat || (iat & 7) || names >= size || iat >= size) return 0;
        bool ended = false;
        const size_t limit = std::min<size_t>(65536, std::min((size - names) / 8, (size - iat) / 8));
        for (size_t i = 0; i < limit; ++i) {
            uint64_t name = 0;
            if (!read(names + i * 8, &name, 8)) return 0;
            if (!name) { ended = true; break; }
            if (name >> 63) continue;  // ordinal import
            if (name > UINT32_MAX || name > size || size - size_t(name) < 9) return 0;
            char symbol[7]{};
            if (!read(size_t(name) + 2, symbol, sizeof symbol)) return 0;
            if (std::memcmp(symbol, "memcmp", sizeof symbol) == 0) slots.push_back(iat + i * 8);
        }
        if (!ended) return 0;
    }
    if (!terminated) return 0;
    // Validate the entire walk before making any changes. No symbol match means no protection change.
    size_t patched = 0;
    for (size_t slot : slots) if (write(image + slot, shim)) ++patched;
    return patched;
}
} // namespace kshroud::original
