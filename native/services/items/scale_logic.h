// SPDX-License-Identifier: MIT
// kiln.items v8 (package P7): the per-instance power and armour scales without game memory (host test
// tests/gear_test.cpp). Table is the one table the hooks read (scale.cpp): written by one thread at a time (the caller's
// mutex), read lock-free by the game's worker threads inside apply_armor and impact_initialize. ABI: kiln_items.h "power
// and armour scales".
//
// Layout: two open-addressing arrays of 16384 slots {key, power bits, armor bits}; one is current. A key, once written
// into a slot, stays there until the next rebuild: setting both scales back to 1 leaves a dead slot (values 1, so a
// reader that finds it changes nothing). Entity ids are never reused within one server world (stamps.h), so a dead slot
// can never describe another item. A rebuild (too many dead slots, or clear()) writes the live entries into the other
// array and then makes it current; the array a reader may still hold is the one written two rebuilds later, and a lookup
// lasts nanoseconds, so a reader never sees an array being rewritten in practice; were it to, it would read a key that
// is written last (after its values), so it finds an entry with its own values or none.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "services/kiln_items.h"

namespace kitems::scale {

constexpr uint32_t kCapacity = KILN_ITEMS_SCALE_CAPACITY;  // live entries
constexpr uint32_t kSlots = 16384;                         // per array; a power of two, twice the capacity
constexpr uint32_t kRebuildAt = kSlots * 3 / 4;            // used slots (live + dead) that start a rebuild
static_assert((kSlots & (kSlots - 1)) == 0 && kSlots >= 2 * kCapacity && kRebuildAt > kCapacity);

inline uint32_t bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
inline float from_bits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline const uint32_t kOne = bits(1.0f);

// A scale a request may set: finite, inside MIN..MAX.
inline bool valid(float f) { return std::isfinite(f) && f >= KILN_ITEMS_SCALE_MIN && f <= KILN_ITEMS_SCALE_MAX; }

// The request checks of gear_scale (the stamp is checked against the stamp table by the caller).
inline bool request_valid(const KilnItemsGearScale& s) {
    constexpr uint32_t kAll = KILN_ITEMS_SCALE_POWER | KILN_ITEMS_SCALE_ARMOR;
    if (s.size < sizeof(KilnItemsGearScale) || s.entity == 0 || s.stamp == 0) return false;
    if ((s.flags & ~kAll) != 0 || (s.flags & kAll) == 0 || s.reserved[0] != 0 || s.reserved[1] != 0) return false;
    if ((s.flags & KILN_ITEMS_SCALE_POWER) && !valid(s.power)) return false;
    if ((s.flags & KILN_ITEMS_SCALE_ARMOR) && !valid(s.armor)) return false;
    return true;
}

// A game value times a scale, rounded to the nearest whole number, kept inside 0..2^30 (the game's values are small;
// a scale of 100 on any of them stays far below).
inline int32_t apply(int32_t value, float scale) {
    const double v = std::floor(static_cast<double>(value) * scale + 0.5);
    if (v <= 0) return 0;
    if (v >= 1073741824.0) return 1073741824;
    return static_cast<int32_t>(v);
}

// What one piece added to the wearer's armour values (after - before), times its scale, added back to before.
inline void apply_armor(int32_t* values, const int32_t* before, uint32_t count, float scale) {
    for (uint32_t i = 0; i < count; ++i) {
        const int64_t added = static_cast<int64_t>(values[i]) - before[i];
        const double scaled = std::floor(static_cast<double>(added) * scale + 0.5);
        double v = static_cast<double>(before[i]) + scaled;
        if (v > 2147483647.0) v = 2147483647.0;
        if (v < -2147483648.0) v = -2147483648.0;
        values[i] = static_cast<int32_t>(v);
    }
}

class Table {
public:
    struct Slot {
        std::atomic<uint32_t> key{0};
        std::atomic<uint32_t> power{0};
        std::atomic<uint32_t> armor{0};
    };

    Table() { cur_.store(a_, std::memory_order_relaxed); }

    // ---- readers (any thread, lock-free) ----
    uint32_t live() const { return live_.load(std::memory_order_acquire); }
    float power(uint32_t entity) const { return read(entity, true); }
    float armor(uint32_t entity) const { return read(entity, false); }

    // ---- the writer (one thread at a time) ----
    // Sets the scales the flags name (KILN_ITEMS_SCALE_*); a value of 1 removes that scale. false: the table is full
    // (kCapacity live entries) and the entry is new; nothing changed.
    bool set(uint32_t entity, uint32_t flags, float power, float armor) {
        Slot* s = cur_.load(std::memory_order_relaxed);
        Slot* e = probe(s, entity);
        const bool had = e && e->key.load(std::memory_order_relaxed) == entity;
        const uint32_t p = (flags & KILN_ITEMS_SCALE_POWER) ? bits(power) : had ? e->power.load(std::memory_order_relaxed) : kOne;
        const uint32_t a = (flags & KILN_ITEMS_SCALE_ARMOR) ? bits(armor) : had ? e->armor.load(std::memory_order_relaxed) : kOne;
        const bool want = p != kOne || a != kOne;
        if (had) {
            const bool was = e->power.load(std::memory_order_relaxed) != kOne || e->armor.load(std::memory_order_relaxed) != kOne;
            e->power.store(p, std::memory_order_release);
            e->armor.store(a, std::memory_order_release);
            if (was != want) live_.fetch_add(want ? 1u : 0xFFFFFFFFu, std::memory_order_acq_rel);
            return true;
        }
        if (!want) return true;  // nothing to remove
        if (live_.load(std::memory_order_relaxed) >= kCapacity) return false;
        if (used_ + 1 > kRebuildAt) {
            rebuild();
            s = cur_.load(std::memory_order_relaxed);
            e = probe(s, entity);
        }
        e->power.store(p, std::memory_order_relaxed);
        e->armor.store(a, std::memory_order_relaxed);
        e->key.store(entity, std::memory_order_release);  // last: a reader that finds the key reads its values
        ++used_;
        live_.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    // Forgets every entry (a server world closed or started).
    void clear() {
        Slot* next = cur_.load(std::memory_order_relaxed) == a_ ? b_ : a_;
        wipe(next);
        live_.store(0, std::memory_order_release);
        cur_.store(next, std::memory_order_release);
        used_ = 0;
    }

    uint32_t used() const { return used_; }  // live + dead slots (the writer's view)

    // Calls fn(entity, power, armor) for every live entry (the writer's thread).
    template <class F>
    void each(F&& fn) const {
        const Slot* s = cur_.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < kSlots; ++i) {
            const uint32_t k = s[i].key.load(std::memory_order_relaxed);
            const uint32_t p = s[i].power.load(std::memory_order_relaxed), a = s[i].armor.load(std::memory_order_relaxed);
            if (k && (p != kOne || a != kOne)) fn(k, from_bits(p), from_bits(a));
        }
    }

private:
    static uint32_t home(uint32_t entity) { return (entity * 0x9E3779B1u) >> 18; }  // 14 bits: kSlots
    static_assert(kSlots == (1u << 14));

    // The slot holding the entity, else the first empty slot of its run; nullptr: no empty slot (never with kRebuildAt).
    static Slot* probe(Slot* s, uint32_t entity) {
        for (uint32_t i = 0, h = home(entity); i < kSlots; ++i, h = (h + 1) & (kSlots - 1)) {
            const uint32_t k = s[h].key.load(std::memory_order_acquire);
            if (k == entity || k == 0) return &s[h];
        }
        return nullptr;
    }

    float read(uint32_t entity, bool power) const {
        if (!entity || live_.load(std::memory_order_acquire) == 0) return 1.0f;
        const Slot* s = cur_.load(std::memory_order_acquire);
        for (uint32_t i = 0, h = home(entity); i < kSlots; ++i, h = (h + 1) & (kSlots - 1)) {
            const uint32_t k = s[h].key.load(std::memory_order_acquire);
            if (k == 0) return 1.0f;
            if (k == entity) return from_bits((power ? s[h].power : s[h].armor).load(std::memory_order_acquire));
        }
        return 1.0f;
    }

    static void wipe(Slot* s) {
        for (uint32_t i = 0; i < kSlots; ++i) {
            s[i].key.store(0, std::memory_order_relaxed);
            s[i].power.store(kOne, std::memory_order_relaxed);
            s[i].armor.store(kOne, std::memory_order_relaxed);
        }
    }

    // The live entries into the other array, which then becomes current; the dead slots are gone.
    void rebuild() {
        Slot* from = cur_.load(std::memory_order_relaxed);
        Slot* to = from == a_ ? b_ : a_;
        wipe(to);
        uint32_t n = 0;
        for (uint32_t i = 0; i < kSlots; ++i) {
            const uint32_t k = from[i].key.load(std::memory_order_relaxed);
            const uint32_t p = from[i].power.load(std::memory_order_relaxed), a = from[i].armor.load(std::memory_order_relaxed);
            if (!k || (p == kOne && a == kOne)) continue;
            Slot* e = probe(to, k);
            e->power.store(p, std::memory_order_relaxed);
            e->armor.store(a, std::memory_order_relaxed);
            e->key.store(k, std::memory_order_relaxed);
            ++n;
        }
        cur_.store(to, std::memory_order_release);
        used_ = n;
    }

    Slot a_[kSlots], b_[kSlots];
    std::atomic<Slot*> cur_{nullptr};
    std::atomic<uint32_t> live_{0};
    uint32_t used_ = 0;
};

}  // namespace kitems::scale
