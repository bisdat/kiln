// SPDX-License-Identifier: MIT
#include "visibility.h"
#include <climits>
#include <cstring>
#include <limits>
namespace kc::registry {
std::optional<FilteredArray> filter_array(uintptr_t data, size_t count, uint32_t stride, uint32_t id_offset,
    const std::vector<uintptr_t>& extra, const std::vector<uint32_t>& relocs, const std::set<uint32_t>& hidden,
    const FilterIo& io, std::string* why) {
    auto fail = [&](const char* reason) -> std::optional<FilteredArray> { *why = reason; return {}; };
    constexpr size_t limit = 1u << 20;
    if (!io.read || !io.allocate || stride < 4 || stride > 65536 || id_offset > stride - 4 || count > limit || extra.size() > limit - count ||
        count > (UINTPTR_MAX - data) / stride) return fail("invalid or oversized entry view");
    for (auto offset : relocs) if (offset > stride - 4) return fail("invalid self-relative entry layout");
    std::vector<uintptr_t> source;
    for (size_t i = 0; i < count + extra.size(); ++i) {
        const uintptr_t at = i < count ? data + i * stride : extra[i - count];
        uint32_t id = 0;
        if (at > UINTPTR_MAX - stride || !io.read(at + id_offset, &id, sizeof id)) return fail("entry view is unreadable");
        if (!hidden.count(id)) source.push_back(at);
    }
    if (source.empty()) return FilteredArray{};
    auto* out = static_cast<uint8_t*>(io.allocate(data, source.size() * stride));
    if (!out) return fail("no memory for the filtered entry view");
    for (size_t i = 0; i < source.size(); ++i) {
        auto* record = out + i * stride;
        if (!io.read(source[i], record, stride)) return fail("entry view is unreadable");
        for (auto offset : relocs) {
            int32_t old = 0; std::memcpy(&old, record + offset, sizeof old);
            if (!old) continue;
            const auto before = source[i] + offset;
            if ((old > 0 && before > UINTPTR_MAX - static_cast<uint32_t>(old)) ||
                (old < 0 && before < static_cast<uint64_t>(-static_cast<int64_t>(old)))) return fail("entry backing is out of range");
            const uintptr_t target = old < 0 ? before - static_cast<uintptr_t>(-static_cast<int64_t>(old)) : before + old;
            const auto after = reinterpret_cast<uintptr_t>(record + offset);
            const uint64_t distance = target >= after ? target - after : after - target;
            if (distance > (target >= after ? uint64_t(INT32_MAX) : uint64_t(INT32_MAX) + 1)) return fail("entry backing is out of reach");
            const int32_t relocated = static_cast<int32_t>(target >= after ? static_cast<int64_t>(distance) : -static_cast<int64_t>(distance));
            std::memcpy(record + offset, &relocated, sizeof relocated);
        }
    }
    return FilteredArray{reinterpret_cast<uintptr_t>(out), source.size()};
}
std::vector<uint64_t> visible_recipes(const std::vector<uint64_t>& recipes, const std::set<uint32_t>& hidden,
    const std::function<std::optional<uint32_t>(uintptr_t)>& id) {
    std::vector<uint64_t> out;
    for (const auto recipe : recipes) {
        const auto value = id(recipe);
        if (value && !hidden.count(*value)) out.push_back(recipe);
    }
    return out;
}
} // namespace kc::registry
