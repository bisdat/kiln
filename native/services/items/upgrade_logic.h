// SPDX-License-Identifier: MIT
// kiln.items v8 upgrades (package P3) without game memory (host test tests/gear_test.cpp): the request checks, how the
// costs are split among the payers, and whether a commit stood (the payers' counts after it). gear.cpp runs them
// inside kiln_items_gear around one inventory transaction of its own. ABI: kiln_items.h "upgrades".
#pragma once
#include <cstdint>

#include "gear_logic.h"
#include "services/kiln_items.h"

namespace kitems::upgrade {

constexpr uint32_t kMaxCosts = KILN_ITEMS_UPGRADE_MAX_COSTS;
constexpr uint32_t kMaxPayers = KILN_ITEMS_UPGRADE_MAX_PAYERS;
constexpr uint32_t kFlags = KILN_ITEMS_UPGRADE_WRITE | KILN_ITEMS_UPGRADE_DRY_RUN;

inline bool request_valid(const KilnItemsUpgradeRequest& r) {
    if (r.size < sizeof(KilnItemsUpgradeRequest) || r.player == 0 || (r.flags & ~kFlags) != 0 || r.expect_available > gear::kPerkSlots)
        return false;
    if (r.cost_count > kMaxCosts || r.payer_count > kMaxPayers || (r.cost_count > 0 && r.payer_count == 0)) return false;
    const KilnItemsGearRequest& g = r.gear;
    if (g.size < sizeof(KilnItemsGearRequest) || g.entity == 0 || g.stamp == 0) return false;
    if (r.flags & KILN_ITEMS_UPGRADE_WRITE) {
        // the write as gear_write takes it; a dry run is the upgrade's own flag, never the write's
        if (!gear::request_valid(g) || (g.flags & KILN_ITEMS_GEAR_DRY_RUN)) return false;
    } else if (g.flags != 0 || g.available != 0 || g.unlocked != 0 || g.level != 0 || g.reserved0 != 0 || g.durability_max != 0 ||
               g.reserved[0] != 0 || g.reserved[1] != 0) {
        return false;  // no value is ever ignored silently
    }
    for (uint32_t i = 0; i < kMaxCosts; ++i) {
        const KilnItemsUpgradeCost& c = r.costs[i];
        if (i >= r.cost_count) {
            if (c.item != 0 || c.count != 0) return false;
            continue;
        }
        if (c.item == 0 || c.count < 1 || c.count > KILN_ITEMS_UPGRADE_MAX_COUNT) return false;
        for (uint32_t k = 0; k < i; ++k)
            if (r.costs[k].item == c.item) return false;
    }
    for (uint32_t i = 0; i < kMaxPayers; ++i) {
        const KilnEntity p = r.payers[i];
        if (i >= r.payer_count) {
            if (p != 0) return false;
            continue;
        }
        if (p == 0) return false;
        for (uint32_t k = 0; k < i; ++k)
            if (r.payers[k] == p) return false;
    }
    return true;
}

// expect_available: the item still has the enhancement slots the caller priced (0: no check).
inline bool as_expected(const KilnItemsUpgradeRequest& r, const KilnItemsGearState& now) {
    if (r.expect_available == 0) return true;
    return (now.flags & KILN_ITEMS_GEAR_HAS_PERKS) && now.available == r.expect_available;
}

// One cost split among the payers in order: each pays what it has until the cost is met.
struct Split {
    uint32_t take[kMaxPayers] = {};
    uint64_t have = 0;  // all payers together
    bool enough = false;
};
inline Split split(uint32_t cost, const uint32_t* have, uint32_t payers) {
    Split s;
    uint32_t left = cost;
    for (uint32_t i = 0; i < payers && i < kMaxPayers; ++i) {
        s.have += have[i];
        const uint32_t t = have[i] < left ? have[i] : left;
        s.take[i] = t;
        left -= t;
    }
    s.enough = left == 0;
    if (!s.enough)
        for (uint32_t i = 0; i < kMaxPayers; ++i) s.take[i] = 0;  // all or nothing: nobody pays a part
    return s;
}

// One op a payment queued on the transaction (docs/re/items.md: 0x34-byte records; +0x26 the kind, 1 = take; +0x30 set when
// the take empties the slot, +0x20 then the stack's item entity, 0 or 0xFFFFFFFF for a plain stack): a payment may only
// take plain stacks. Lab dev29-eq-p3 (server 26430253): a take that emptied a plain stack of twigs has +0x30 set.
inline bool plain_take(uint8_t kind, uint8_t empties, uint32_t pide) {
    if (kind != 1) return false;
    return !empties || pide == 0 || pide == 0xFFFFFFFFu;
}

// After the commit: every cost came out of the payers in full (before - after == cost for each). False: the game's
// commit rolled back by itself, or took something else; then nothing may be written.
inline bool commit_stood(const uint64_t* before, const uint64_t* after, const KilnItemsUpgradeCost* costs, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (after[i] > before[i] || before[i] - after[i] != costs[i].count) return false;
    return true;
}

}  // namespace kitems::upgrade
