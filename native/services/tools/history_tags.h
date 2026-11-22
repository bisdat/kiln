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
    class Reservation {
    public:
        Reservation() = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        ~Reservation() { if (owner_) owner_->release(slot_, generation_); }
        explicit operator bool() const { return owner_ != nullptr; }
        bool created(uintptr_t entry) {
            if (!owner_ || !owner_->commit(slot_, generation_, entry)) return false;
            owner_ = nullptr;
            return true;
        }
    private:
        friend class HistoryTags;
        Reservation(HistoryTags* owner, size_t slot, uint64_t generation)
            : owner_(owner), slot_(slot), generation_(generation) {}
        HistoryTags* owner_ = nullptr;
        size_t slot_ = 0;
        uint64_t generation_ = 0;
    };
    // Reserve before invoking the native allocator: it can evict an older entry even if the new action later fails.
    // Reservations are distinct from exact identities and remain owned across nested/concurrent native calls.
    Reservation reserve() {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < Capacity; ++i) {
            if (!lists_[i] && !reserved_[i]) {
                reserved_[i] = true;
                return Reservation(this, i, generation_);
            }
        }
        return {};
    }
    bool created(uintptr_t entry, bool policy) {
        if (!valid(entry)) return false;
        const auto list = entry + kMaterials;
        std::lock_guard lock(mutex_);
        uintptr_t* free = nullptr;
        for (size_t i = 0; i < Capacity; ++i) {
            auto& slot = lists_[i];
            if (slot == list) slot = 0;
            if (!slot && !reserved_[i] && !free) free = &slot;
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
        reserved_.fill(false);
        ++generation_;
    }
private:
    static constexpr uintptr_t kMaterials = 0x110;
    static bool valid(uintptr_t entry) {
        return entry && entry <= std::numeric_limits<uintptr_t>::max() - kMaterials;
    }
    bool commit(size_t index, uint64_t generation, uintptr_t entry) {
        if (!valid(entry)) return false;
        std::lock_guard lock(mutex_);
        if (generation != generation_ || !reserved_[index]) return false;
        const auto list = entry + kMaterials;
        for (auto& slot : lists_) if (slot == list) slot = 0;
        lists_[index] = list;
        reserved_[index] = false;
        return true;
    }
    void release(size_t index, uint64_t generation) {
        std::lock_guard lock(mutex_);
        if (generation == generation_) reserved_[index] = false;
    }
    mutable std::mutex mutex_;
    std::array<uintptr_t, Capacity> lists_{};
    std::array<bool, Capacity> reserved_{};
    uint64_t generation_ = 0;
};
}
