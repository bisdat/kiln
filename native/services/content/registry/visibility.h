// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace kc::registry {
struct Visibility {
    std::set<uint32_t> items, recipes, loot;
    uint64_t generation = 0;
};
struct FilteredArray { uintptr_t data = 0; size_t count = 0; };
struct FilterIo {
    std::function<bool(uintptr_t, void*, size_t)> read;
    std::function<void*(uintptr_t, size_t)> allocate;
};
// Build a new immutable array, retaining original records and rebasing every self-relative field.
// Empty output is valid; allocation/read/relocation failure never returns the unfiltered input.
std::optional<FilteredArray> filter_array(uintptr_t data, size_t count, uint32_t stride, uint32_t id_offset,
    const std::vector<uintptr_t>& extra, const std::vector<uint32_t>& relocs, const std::set<uint32_t>& hidden,
    const FilterIo& io, std::string* why);
std::vector<uint64_t> visible_recipes(const std::vector<uint64_t>& recipes, const std::set<uint32_t>& hidden,
    const std::function<std::optional<uint32_t>(uintptr_t)>& id);
} // namespace kc::registry
