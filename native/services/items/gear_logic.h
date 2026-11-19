// SPDX-License-Identifier: MIT
// kiln.items v8: gear writes without game memory (host test tests/gear_test.cpp). What a KilnItemsGearRequest does to an
// item entity's PerkContainerNew, Level, Durability and ItemState, decided from what the program read; gear.cpp then
// makes the writes the plan names. Rules: kiln_items.h "gear", evidence docs/re/items.md section 19.
#pragma once
#include <cstdint>

#include "services/kiln_items.h"

namespace kitems::gear {

constexpr uint32_t kPerkSlots = 5;    // ItemInfo.perkIds[5] / perkReferences[5]; ItemRarity Common 0 .. Legendary 4
constexpr uint32_t kMaxLevel = 255;   // the save record keeps the Level's low byte (docs/re/items.md section 10)

constexpr uint32_t kOps = KILN_ITEMS_GEAR_AVAILABLE | KILN_ITEMS_GEAR_UNLOCKED | KILN_ITEMS_GEAR_LEVEL |
                          KILN_ITEMS_GEAR_DURABILITY_REFRESH | KILN_ITEMS_GEAR_DURABILITY_REPAIR;
constexpr uint32_t kFlags = kOps | KILN_ITEMS_GEAR_FORCE_SLOT | KILN_ITEMS_GEAR_DRY_RUN | KILN_ITEMS_GEAR_NOT_EQUIPPED;

// The type's last filled perk slot + 1 (perkIds[i] != 0), 0 when none is filled. A gap below it does not stop it: the
// game's perk programs skip an empty slot (equipment_impacts, apply_armor test the registry lookup).
inline uint8_t max_available(const uint32_t (&perk_ids)[kPerkSlots]) {
    uint8_t n = 0;
    for (uint32_t i = 0; i < kPerkSlots; ++i)
        if (perk_ids[i] != 0) n = static_cast<uint8_t>(i + 1);
    return n;
}

// The game's itemRarityOf (server 0x1bb220): availablePerks - 1 when that is 0..4, else the type's rarity.
inline uint8_t rarity_of(bool has_perks, uint8_t available, uint8_t type_rarity) {
    if (has_perks && available >= 1 && available <= kPerkSlots) return static_cast<uint8_t>(available - 1);
    return type_rarity;
}

inline bool request_valid(const KilnItemsGearRequest& r) {
    if (r.size < sizeof(KilnItemsGearRequest) || r.entity == 0 || r.stamp == 0) return false;
    if ((r.flags & ~kFlags) != 0 || (r.flags & kOps) == 0) return false;
    if (r.reserved0 != 0 || r.reserved[0] != 0 || r.reserved[1] != 0 || r.reserved[2] != 0) return false;
    // a field whose operation is not asked for must be 0: no value is ever ignored silently
    if (r.flags & KILN_ITEMS_GEAR_AVAILABLE) {
        if (r.available < 1 || r.available > kPerkSlots) return false;
    } else if (r.available != 0) {
        return false;
    }
    if (r.flags & KILN_ITEMS_GEAR_UNLOCKED) {
        if (r.unlocked > kPerkSlots) return false;
    } else if (r.unlocked != 0) {
        return false;
    }
    if (r.flags & KILN_ITEMS_GEAR_LEVEL) {
        if (r.level < 1 || r.level > kMaxLevel) return false;
    } else if (r.level != 0) {
        return false;
    }
    return true;
}

struct Plan {
    int32_t status = KILN_ITEMS_GEAR_DONE;
    const char* why = "";
    KilnItemsGearState after{};
    uint32_t changed = 0;          // KILN_ITEMS_GEAR_CHANGED_*
    bool write_perks = false;      // PerkContainerNew := after.available / after.unlocked
    bool write_level = false;      // Level := after.level, max := after.level_max
    bool write_durability = false; // Durability current := after.durability
    bool clear_init = false;       // ItemState.itemState &= ~init_bit
};

inline Plan refuse(Plan p, const KilnItemsGearState& before, int32_t status, const char* why) {
    p.status = status;
    p.why = why;
    p.after = before;
    p.changed = 0;
    p.write_perks = p.write_level = p.write_durability = p.clear_init = false;
    return p;
}

// before: what the program read (flags, values, the type's facts). init_bit / equipped_bit: ItemStateFlag
// DurabilityInitialized and IsEquipped as masks of ItemState.itemState (this build's reflection).
//  - AVAILABLE sets the slots; beyond the type's last filled slot only with FORCE_SLOT. Lowering it below the unlocked
//    count brings the unlocked count down with it, as the game's free -1 action does one at a time.
//  - Every change of the unlocked count moves Level by the same number (UnlockPerk: unlocked + 1, Level + 1; Level's
//    max is raised first when the new level would pass it). LEVEL sets Level outright; its base (level - unlocked) must
//    be in the type's level range: a level outside it is not kept by a save load (the game re-clamps it).
//  - REPAIR puts current durability at max; REPAIR and REFRESH clear DurabilityInitialized, so the game recomputes max
//    at the new rarity and level in the next step, keeping the ratio. An item without Durability has nothing to do.
inline Plan plan(const KilnItemsGearRequest& r, const KilnItemsGearState& before, uint16_t init_bit, uint16_t equipped_bit) {
    Plan p;
    p.after = before;
    const bool perks = (before.flags & KILN_ITEMS_GEAR_HAS_PERKS) != 0;
    const bool level = (before.flags & KILN_ITEMS_GEAR_HAS_LEVEL) != 0;
    const bool durability = (before.flags & KILN_ITEMS_GEAR_HAS_DURABILITY) != 0;
    if ((r.flags & KILN_ITEMS_GEAR_NOT_EQUIPPED) &&
        ((before.flags & KILN_ITEMS_GEAR_EQUIPPED_NOW) || (equipped_bit && (before.state & equipped_bit))))
        return refuse(p, before, KILN_ITEMS_GEAR_EQUIPPED, "the item is equipped");
    const bool slots_op = (r.flags & (KILN_ITEMS_GEAR_AVAILABLE | KILN_ITEMS_GEAR_UNLOCKED)) != 0;
    if (slots_op && !perks)
        return refuse(p, before, KILN_ITEMS_GEAR_NOT_GEAR, "the item has no enhancement slots (no PerkContainerNew)");

    uint8_t avail = before.available, unl = before.unlocked;
    if (r.flags & KILN_ITEMS_GEAR_AVAILABLE) {
        if (r.available > before.available && r.available > before.max_available && !(r.flags & KILN_ITEMS_GEAR_FORCE_SLOT))
            return refuse(p, before, KILN_ITEMS_GEAR_NO_SLOT, "the item's type has no perk in that enhancement slot");
        avail = r.available;
        if (unl > avail) unl = avail;
    }
    if (r.flags & KILN_ITEMS_GEAR_UNLOCKED) {
        if (r.unlocked > avail)
            return refuse(p, before, KILN_ITEMS_GEAR_OUT_OF_RANGE, "more enhancements unlocked than the item has slots");
        unl = r.unlocked;
    }
    const int64_t delta = static_cast<int64_t>(unl) - static_cast<int64_t>(before.unlocked);
    int64_t new_level = before.level;
    if (r.flags & KILN_ITEMS_GEAR_LEVEL) {
        if (!level) return refuse(p, before, KILN_ITEMS_GEAR_NOT_GEAR, "the item has no Level");
        const int64_t base = static_cast<int64_t>(r.level) - unl;
        if (base < 1 || base < before.type_level_min || base > before.type_level_max)
            return refuse(p, before, KILN_ITEMS_GEAR_OUT_OF_RANGE, "the level's base is outside the type's level range");
        new_level = r.level;
    } else if (delta != 0) {
        if (!level) return refuse(p, before, KILN_ITEMS_GEAR_NOT_GEAR, "the item has no Level for its unlocked enhancements");
        new_level += delta;
        if (new_level < 1 || new_level > kMaxLevel)
            return refuse(p, before, KILN_ITEMS_GEAR_OUT_OF_RANGE, "the level would leave 1..255");
    }

    p.after.available = avail;
    p.after.unlocked = unl;
    p.after.rarity = rarity_of(perks, avail, before.type_rarity);
    if (perks && (avail != before.available || unl != before.unlocked)) {
        p.write_perks = true;
        p.changed |= KILN_ITEMS_GEAR_CHANGED_PERKS;
    }
    if (level) {
        p.after.level = static_cast<uint32_t>(new_level);
        p.after.level_max = p.after.level > before.level_max ? p.after.level : before.level_max;
        if (p.after.level != before.level || p.after.level_max != before.level_max) {
            p.write_level = true;
            p.changed |= KILN_ITEMS_GEAR_CHANGED_LEVEL;
        }
    }
    if (durability && (r.flags & (KILN_ITEMS_GEAR_DURABILITY_REFRESH | KILN_ITEMS_GEAR_DURABILITY_REPAIR))) {
        if ((r.flags & KILN_ITEMS_GEAR_DURABILITY_REPAIR) && before.durability != before.durability_max) {
            p.after.durability = before.durability_max;
            p.write_durability = true;
            p.changed |= KILN_ITEMS_GEAR_CHANGED_DURABILITY;
        }
        if (init_bit && (before.state & init_bit)) {
            p.after.state = static_cast<uint16_t>(before.state & ~init_bit);
            p.after.flags &= ~KILN_ITEMS_GEAR_DURABILITY_INIT;
            p.clear_init = true;
            p.changed |= KILN_ITEMS_GEAR_CHANGED_STATE;
        }
    }
    return p;
}

}  // namespace kitems::gear
