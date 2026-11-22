// SPDX-License-Identifier: MIT
// EMF API v31 hits on Kiln, the part without the game (host tests: tests/hits_test.cpp).
//  - hit_from_event: one keen::ecs::HitEvent record (248 bytes on the builds Kiln knows) as EMF's EmfHit, as EMF's own
//    core reads it (EMF src/core/hits.cpp common_fields + hit_from_event): attacker = rootSourceId, part = targetId,
//    target = rootTargetId or else part, amount = -healthChange, stamina = -staminaChange, the game's flags, damage
//    types, attack kinds, weapon category, program, impact, reaction, target faction and position. Terrain and
//    building hits are never passed. health_before is -1 (EMF: "after the hit ... health_before -1"); health_after and
//    weapon_item are filled by the caller from the world.
//  - Observers: the on_hit callbacks (at most 16, as EMF), each with its mod; a callback that faults is switched off.
// Offsets: EMF's generated layout (sdk/include/emf/generated/components.hpp, HitEvent) = the first 0xF8 bytes of
// kiln.combat's HitCandidate (native/services/combat/combat_internal.h, HitOffsets).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "emf/emf_api.h"

namespace kemf::hits {

struct HitEventLayout {
    static constexpr uint32_t size = 248;  // sizeof(keen::ecs::HitEvent)
    static constexpr size_t program = 88, position = 108, root_source = 144, target = 148, flags = 152, root_target = 160,
                            source = 164, weapon_pide = 172, health_change = 188, stamina_change = 192, impact = 212,
                            weapon_category = 216, damage_types = 220, attack_category = 223, reaction = 224,
                            target_faction = 240;
};
// impact ids of kiln.combat damage() hits (EMF's damage() on Kiln goes through it): 0xCBxxxxxx
constexpr uint32_t kKilnImpactTag = 0xCB000000u;
constexpr uint32_t kMaxObservers = 16;

// false: too short, or a terrain or building hit (EMF passes neither)
bool hit_from_event(const void* event, uint32_t size, EmfHit* out);
// the HitEvent's weaponPideId (the weapon item entity), 0 when the record is too short
uint32_t weapon_pide(const void* event, uint32_t size);

class Observers {
public:
    struct Entry {
        void* mod = nullptr;  // kemf::Mod*, opaque here
        EmfHitCallback cb = nullptr;
        void* user = nullptr;
        std::atomic<bool> on{false};
    };
    // false for a null callback or when 16 are registered
    bool add(void* mod, EmfHitCallback cb, void* user);
    uint32_t count() const { return n_.load(std::memory_order_acquire); }
    // callbacks still switched on: 0 = nobody listens (the relay then skips the game's queue)
    uint32_t live() const { return live_.load(std::memory_order_acquire); }
    // calls run(entry) for every switched-on entry in registration order; run returns false when the callback faulted,
    // which switches that entry off (true is returned once per entry that went off, for the log line)
    template <class Run, class Off>
    void each(Run&& run, Off&& off) {
        const uint32_t n = count();  // entries are only appended: index i is final once counted
        for (uint32_t i = 0; i < n; ++i) {
            Entry& e = entries_[i];
            if (!e.on.load(std::memory_order_acquire)) continue;
            if (!run(e) && e.on.exchange(false)) {
                live_.fetch_sub(1, std::memory_order_acq_rel);
                off(e);
            }
        }
    }
    void reset();  // tests only

private:
    mutable std::mutex mu_;
    Entry entries_[kMaxObservers];
    std::atomic<uint32_t> n_{0}, live_{0};
};

}  // namespace kemf::hits
