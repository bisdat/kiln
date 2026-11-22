// SPDX-License-Identifier: MIT
// EMF API v31 hits on Kiln without the game: see hits_model.h.
#include "hits_model.h"

namespace kemf::hits {

namespace {

template <class T>
T rd(const uint8_t* p, size_t off) {
    T v{};
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

}  // namespace

bool hit_from_event(const void* event, uint32_t size, EmfHit* h) {
    using L = HitEventLayout;
    if (!event || !h || size < L::size) return false;
    const auto* p = static_cast<const uint8_t*>(event);
    const uint32_t flags = rd<uint32_t>(p, L::flags);
    if (flags & (EMF_HIT_TERRAIN | EMF_HIT_BUILDING)) return false;
    *h = EmfHit{};
    h->size = sizeof(EmfHit);
    h->attacker = rd<uint32_t>(p, L::root_source);
    h->source = rd<uint32_t>(p, L::source);
    h->part = rd<uint32_t>(p, L::target);
    const uint32_t root = rd<uint32_t>(p, L::root_target);
    h->target = root ? root : h->part;
    // the game's change is negative for damage; EMF's amount is positive for damage (unsigned negation: INT_MIN stays)
    h->amount = static_cast<int32_t>(0u - static_cast<uint32_t>(rd<int32_t>(p, L::health_change)));
    h->stamina = static_cast<int32_t>(0u - static_cast<uint32_t>(rd<int32_t>(p, L::stamina_change)));
    h->flags = flags;
    h->damage_types = rd<uint16_t>(p, L::damage_types);
    h->attacks = rd<uint8_t>(p, L::attack_category);
    h->kind = EMF_HIT_KIND_HIT;
    h->weapon_category = rd<uint32_t>(p, L::weapon_category);
    h->program = rd<uint32_t>(p, L::program);
    h->impact = rd<uint32_t>(p, L::impact);
    h->reaction = rd<uint16_t>(p, L::reaction);
    h->target_faction = rd<uint8_t>(p, L::target_faction);
    h->from_mod = (h->impact & 0xFF000000u) == kKilnImpactTag ? 1 : 0;
    h->health_before = -1;
    h->health_after = -1;
    std::memcpy(h->position, p + L::position, sizeof h->position);
    return true;
}

uint32_t weapon_pide(const void* event, uint32_t size) {
    if (!event || size < HitEventLayout::size) return 0;
    return rd<uint32_t>(static_cast<const uint8_t*>(event), HitEventLayout::weapon_pide);
}

bool Observers::add(void* mod, EmfHitCallback cb, void* user) {
    if (!cb) return false;
    std::lock_guard lock(mu_);
    const uint32_t n = n_.load(std::memory_order_relaxed);
    if (n >= kMaxObservers) return false;
    Entry& e = entries_[n];
    e.mod = mod;
    e.cb = cb;
    e.user = user;
    e.on.store(true, std::memory_order_release);
    live_.fetch_add(1, std::memory_order_acq_rel);
    n_.store(n + 1, std::memory_order_release);  // published last: a reader that counts it sees the whole entry
    return true;
}

void Observers::reset() {
    std::lock_guard lock(mu_);
    for (auto& e : entries_) {
        e.mod = nullptr;
        e.cb = nullptr;
        e.user = nullptr;
        e.on.store(false);
    }
    n_.store(0);
    live_.store(0);
}

}  // namespace kemf::hits
