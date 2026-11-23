// SPDX-License-Identifier: MIT
// The entity ids of a world's entity map for for_each_entity (0.2.0-dev.30.7). The map is a keen hash map with a fixed
// slot count (1,048,576 in the server world of a hosted game: kiln.spawn's "entity map readable" line), of which a
// world uses a few ten thousand. Up to 30.6 for_each_entity copied every slot's key (4 MB, a fresh allocation each
// call) and tested the slots one by one; Lua mods that call world:entities() every step paid that per step. This copies
// only the occupancy bits (cap / 8 bytes, 128 KB, into a buffer the caller keeps) and then the keys of occupied slots:
// one 4-byte read per entity, or the slot block's 64 keys when kBlockAt or more of them are occupied.
//
// copy(dst, src_address, bytes) -> bool reads game memory under a guard (kiln-emf: seh_copy). Portable: the host test is
// tests/entity_scan_test.cpp (also a benchmark against the old way).
#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kemf {

inline constexpr int kBlockAt = 8;  // occupied slots in a 64-slot block from which one block copy beats single reads

// Appends the key of every occupied slot of a map with `cap` slots (a power of two) to `out`, in slot order. False when
// a read failed (then `out` may hold a part of the ids: the caller drops them, as the old code called nothing).
template <class Copy>
bool scan_entity_keys(uint64_t cap, uintptr_t bits, uintptr_t keys, Copy&& copy, std::vector<uint64_t>& words,
                      std::vector<uint32_t>& out) {
    if (cap == 0 || !bits || !keys) return false;
    const size_t n = static_cast<size_t>((cap + 63) / 64);
    words.resize(n);
    if (!copy(words.data(), bits, n * 8)) return false;
    uint32_t block[64];
    for (size_t w = 0; w < n; ++w) {
        uint64_t word = words[w];
        if (!word) continue;
        const uint64_t first = static_cast<uint64_t>(w) * 64;
        const uint64_t slots = std::min<uint64_t>(64, cap - first);
        if (slots < 64) word &= (uint64_t{1} << slots) - 1;  // bits past the last slot are not slots
        const uintptr_t base = keys + static_cast<uintptr_t>(first) * 4;
        if (std::popcount(word) >= kBlockAt) {
            if (!copy(block, base, static_cast<size_t>(slots) * 4)) return false;
            while (word) {
                const unsigned b = static_cast<unsigned>(std::countr_zero(word));
                word &= word - 1;
                out.push_back(block[b]);
            }
        } else {
            while (word) {
                const unsigned b = static_cast<unsigned>(std::countr_zero(word));
                word &= word - 1;
                uint32_t id = 0;
                if (!copy(&id, base + b * 4u, 4)) return false;
                out.push_back(id);
            }
        }
    }
    return true;
}

}  // namespace kemf
