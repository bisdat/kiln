// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace kiln::tools {
// The native history owns both the entry and the materials. Only the descriptor's identity is retained here;
// no borrowed cell, inventory or list-data pointers are dereferenced. Every native entry creation invalidates the
// old identity before marking a policy entry, including ring slots subsequently reused by vanilla actions.
// Layout evidence: docs/re/gameplay.md, player building history (server 26430253, client 26430625).
template <size_t Capacity = 2048>
class HistoryTags {
public:
    bool created(uintptr_t entry, bool policy) {
        if (!entry || entry > std::numeric_limits<uintptr_t>::max() - kMaterials) return false;
        const auto list = entry + kMaterials;
        std::lock_guard lock(mutex_);
        uintptr_t* free = nullptr;
        for (auto& slot : lists_) {
            if (slot == list) slot = 0;
            if (!slot && !free) free = &slot;
        }
        if (!policy) return true;
        if (!free) return false;
        *free = list;
        return true;
    }
    bool exact(uintptr_t list) const {
        if (!list) return false;
        std::lock_guard lock(mutex_);
        for (const auto slot : lists_) if (slot == list) return true;
        return false;
    }
    void reset() {
        std::lock_guard lock(mutex_);
        lists_.fill(0);
    }
private:
    static constexpr uintptr_t kMaterials = 0x110;
    mutable std::mutex mutex_;
    std::array<uintptr_t, Capacity> lists_{};
};
}
