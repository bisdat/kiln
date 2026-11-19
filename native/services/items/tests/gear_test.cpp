// SPDX-License-Identifier: MIT
// kiln.items v8 host test: gear writes without game memory (gear_logic.h): the request checks, the game's itemRarityOf,
// the type's last filled perk slot, and what a request plans for PerkContainerNew, Level, Durability and ItemState.
#include <cstdio>
#include <cstring>

#include "../gear_logic.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

using namespace kitems::gear;

constexpr uint16_t kInit = 0x02;      // ItemStateFlag DurabilityInitialized (value 1)
constexpr uint16_t kEquipped = 0x08;  // IsEquipped (value 3)

KilnItemsGearRequest req(uint32_t flags) {
    KilnItemsGearRequest r{};
    r.size = sizeof r;
    r.flags = flags;
    r.entity = 5000;
    r.stamp = 7;
    return r;
}

// A Rare sword: 3 of 5 slots available, none unlocked, level 10 (range 5..55), durability 120 / 160, initialized.
KilnItemsGearState rare_sword() {
    KilnItemsGearState s{};
    s.flags = KILN_ITEMS_GEAR_HAS_PERKS | KILN_ITEMS_GEAR_HAS_LEVEL | KILN_ITEMS_GEAR_HAS_DURABILITY | KILN_ITEMS_GEAR_DURABILITY_INIT;
    s.item = 0x1234;
    s.available = 3;
    s.unlocked = 0;
    s.rarity = 2;
    s.type_rarity = 2;
    s.max_available = 5;
    s.level = 10;
    s.level_max = 10;
    s.type_level_min = 5;
    s.type_level_max = 55;
    s.durability = 120;
    s.durability_max = 160;
    s.state = kInit;
    return s;
}

void requests() {
    check(request_valid(req(KILN_ITEMS_GEAR_DURABILITY_REFRESH)), "request: a refresh alone is a request");
    KilnItemsGearRequest r = req(KILN_ITEMS_GEAR_AVAILABLE);
    r.available = 4;
    check(request_valid(r), "request: AVAILABLE 4");
    KilnItemsGearRequest x = r;
    x.entity = 0;
    check(!request_valid(x), "request: entity 0 is refused");
    x = r;
    x.stamp = 0;
    check(!request_valid(x), "request: stamp 0 is refused");
    x = r;
    x.size = sizeof x - 4;
    check(!request_valid(x), "request: a short struct is refused");
    check(!request_valid(req(0)), "request: no operation is refused");
    check(!request_valid(req(KILN_ITEMS_GEAR_DRY_RUN | KILN_ITEMS_GEAR_NOT_EQUIPPED)), "request: only modifiers is no operation");
    x = r;
    x.flags |= 0x100;
    check(!request_valid(x), "request: an unknown flag is refused");
    x = r;
    x.available = 0;
    check(!request_valid(x), "request: AVAILABLE 0 is refused");
    x.available = 6;
    check(!request_valid(x), "request: AVAILABLE 6 is refused");
    x = req(KILN_ITEMS_GEAR_UNLOCKED);
    x.unlocked = 5;
    check(request_valid(x), "request: UNLOCKED 5");
    x.unlocked = 6;
    check(!request_valid(x), "request: UNLOCKED 6 is refused");
    x = req(KILN_ITEMS_GEAR_LEVEL);
    x.level = 0;
    check(!request_valid(x), "request: LEVEL 0 is refused");
    x.level = 256;
    check(!request_valid(x), "request: LEVEL 256 is refused (the save keeps one byte)");
    x.level = 255;
    check(request_valid(x), "request: LEVEL 255");
    x = r;
    x.reserved0 = 1;
    check(!request_valid(x), "request: reserved0 is refused");
    x = r;
    x.reserved[2] = 1;
    check(!request_valid(x), "request: reserved[2] is refused");
    x = req(KILN_ITEMS_GEAR_DURABILITY_REFRESH);
    x.available = 3;
    check(!request_valid(x), "request: available without AVAILABLE is refused (no silent field)");
}

void rarity() {
    check(rarity_of(true, 3, 0) == 2, "itemRarityOf: 3 available is Rare");
    check(rarity_of(true, 1, 4) == 0, "itemRarityOf: 1 available is Common, whatever the type says");
    check(rarity_of(true, 5, 0) == 4, "itemRarityOf: 5 available is Legendary");
    check(rarity_of(true, 0, 3) == 3, "itemRarityOf: 0 available falls back to the type's rarity");
    check(rarity_of(true, 6, 1) == 1, "itemRarityOf: 6 available falls back to the type's rarity");
    check(rarity_of(false, 3, 1) == 1, "itemRarityOf: no PerkContainerNew is the type's rarity");
    const uint32_t all[5] = {1, 2, 3, 4, 5}, three[5] = {1, 2, 3, 0, 0}, gap[5] = {1, 0, 3, 0, 0}, none[5] = {0, 0, 0, 0, 0},
                   last[5] = {0, 0, 0, 0, 9};
    check(max_available(all) == 5, "slots: all five filled");
    check(max_available(three) == 3, "slots: three filled");
    check(max_available(gap) == 3, "slots: the last filled one counts, a gap does not stop it");
    check(max_available(none) == 0, "slots: none filled");
    check(max_available(last) == 5, "slots: only the fifth filled");
}

void plans() {
    const KilnItemsGearState s = rare_sword();
    KilnItemsGearRequest r = req(KILN_ITEMS_GEAR_AVAILABLE);
    r.available = 4;
    Plan p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.write_perks && p.after.available == 4 && p.after.unlocked == 0 &&
              p.after.rarity == 3 && p.changed == KILN_ITEMS_GEAR_CHANGED_PERKS && !p.write_level && !p.clear_init,
          "Rare to Epic: one more slot, nothing unlocked, rarity follows, nothing else");

    r.flags |= KILN_ITEMS_GEAR_DURABILITY_REFRESH;
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.clear_init && (p.after.state & kInit) == 0 &&
              p.changed == (KILN_ITEMS_GEAR_CHANGED_PERKS | KILN_ITEMS_GEAR_CHANGED_STATE) && !p.write_durability,
          "Rare to Epic with REFRESH: DurabilityInitialized cleared, current untouched");

    KilnItemsGearState few = s;
    few.max_available = 3;
    r = req(KILN_ITEMS_GEAR_AVAILABLE);
    r.available = 4;
    p = plan(r, few, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_NO_SLOT && !p.write_perks && p.changed == 0, "no fourth slot on the type: NO_SLOT, nothing written");
    r.flags |= KILN_ITEMS_GEAR_FORCE_SLOT;
    p = plan(r, few, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.after.available == 4, "FORCE_SLOT: the empty slot is allowed");
    r = req(KILN_ITEMS_GEAR_AVAILABLE);
    r.available = 2;
    p = plan(r, few, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.after.available == 2, "lowering never needs a slot check");

    KilnItemsGearState up = s;
    up.available = 4;
    up.unlocked = 3;
    up.level = 13;
    up.level_max = 13;
    r = req(KILN_ITEMS_GEAR_AVAILABLE);
    r.available = 2;
    p = plan(r, up, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.after.available == 2 && p.after.unlocked == 2 && p.after.level == 12 &&
              p.after.level_max == 13 && p.write_perks && p.write_level &&
              p.changed == (KILN_ITEMS_GEAR_CHANGED_PERKS | KILN_ITEMS_GEAR_CHANGED_LEVEL),
          "Epic 3 unlocked to Uncommon: unlocked 2, level 12, max kept");

    r = req(KILN_ITEMS_GEAR_UNLOCKED);
    r.unlocked = 2;
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.after.unlocked == 2 && p.after.level == 12 && p.after.level_max == 12 &&
              p.write_level && p.write_perks,
          "unlock 2: level 12, Level's max raised with it (as UnlockPerk does)");
    r.unlocked = 4;
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_OUT_OF_RANGE && !p.write_perks && !p.write_level, "unlocked above available: OUT_OF_RANGE");
    r = req(KILN_ITEMS_GEAR_AVAILABLE | KILN_ITEMS_GEAR_UNLOCKED);
    r.available = 5;
    r.unlocked = 5;
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.after.available == 5 && p.after.unlocked == 5 && p.after.level == 15 &&
              p.after.rarity == 4,
          "AVAILABLE and UNLOCKED together: Legendary, all unlocked, level 15");

    KilnItemsGearState nolevel = s;
    nolevel.flags &= ~KILN_ITEMS_GEAR_HAS_LEVEL;
    r = req(KILN_ITEMS_GEAR_UNLOCKED);
    r.unlocked = 1;
    p = plan(r, nolevel, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_NOT_GEAR && !p.write_perks, "an unlock without a Level: NOT_GEAR");
    r = req(KILN_ITEMS_GEAR_AVAILABLE);
    r.available = 4;
    p = plan(r, nolevel, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.write_perks && !p.write_level, "a rarity change needs no Level");

    KilnItemsGearState noperks = s;
    noperks.flags &= ~KILN_ITEMS_GEAR_HAS_PERKS;
    noperks.available = noperks.unlocked = 0;
    p = plan(r, noperks, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_NOT_GEAR && !p.write_perks, "no PerkContainerNew: NOT_GEAR");

    r = req(KILN_ITEMS_GEAR_LEVEL);
    r.level = 30;
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.after.level == 30 && p.after.level_max == 30 && p.write_level &&
              p.changed == KILN_ITEMS_GEAR_CHANGED_LEVEL,
          "LEVEL 30 in range 5..55");
    r.level = 4;
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_OUT_OF_RANGE && !p.write_level, "LEVEL 4 below the type's 5: OUT_OF_RANGE");
    r.level = 56;
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_OUT_OF_RANGE, "LEVEL 56 above the type's 55: OUT_OF_RANGE");
    r.level = 58;
    p = plan(r, up, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.after.level == 58, "LEVEL 58 with 3 unlocked: base 55 is in range");
    r.level = 2;
    p = plan(r, up, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_OUT_OF_RANGE, "LEVEL below the unlocked count: OUT_OF_RANGE");
    r.level = 20;
    p = plan(r, nolevel, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_NOT_GEAR, "LEVEL without a Level: NOT_GEAR");
    r = req(KILN_ITEMS_GEAR_UNLOCKED | KILN_ITEMS_GEAR_LEVEL);
    r.unlocked = 2;
    r.level = 20;
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.after.unlocked == 2 && p.after.level == 20, "UNLOCKED with LEVEL: LEVEL wins, base 18");

    KilnItemsGearState odd = s;
    odd.unlocked = 3;
    odd.level = 2;
    r = req(KILN_ITEMS_GEAR_UNLOCKED);
    r.unlocked = 0;
    p = plan(r, odd, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_OUT_OF_RANGE && !p.write_level, "a Level that would go below 1: OUT_OF_RANGE, nothing written");

    KilnItemsGearState worn = s;
    worn.flags |= KILN_ITEMS_GEAR_EQUIPPED_NOW;
    worn.state |= kEquipped;
    r = req(KILN_ITEMS_GEAR_AVAILABLE);
    r.available = 4;
    p = plan(r, worn, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.write_perks, "equipped: allowed by default");
    r.flags |= KILN_ITEMS_GEAR_NOT_EQUIPPED;
    p = plan(r, worn, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_EQUIPPED && !p.write_perks, "equipped with NOT_EQUIPPED: EQUIPPED");

    r = req(KILN_ITEMS_GEAR_DURABILITY_REPAIR);
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.write_durability && p.after.durability == 160 && p.clear_init &&
              p.changed == (KILN_ITEMS_GEAR_CHANGED_DURABILITY | KILN_ITEMS_GEAR_CHANGED_STATE),
          "REPAIR: current to max, then refreshed");
    KilnItemsGearState full = s;
    full.durability = 160;
    p = plan(r, full, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && !p.write_durability && p.clear_init && p.changed == KILN_ITEMS_GEAR_CHANGED_STATE,
          "REPAIR at full: only the refresh");
    KilnItemsGearState nodur = s;
    nodur.flags &= ~KILN_ITEMS_GEAR_HAS_DURABILITY;
    p = plan(r, nodur, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && !p.write_durability && !p.clear_init && p.changed == 0,
          "REPAIR on an item without durability: nothing to do, no refusal");
    KilnItemsGearState fresh = s;
    fresh.state = 0;
    fresh.flags &= ~KILN_ITEMS_GEAR_DURABILITY_INIT;
    p = plan(req(KILN_ITEMS_GEAR_DURABILITY_REFRESH), fresh, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && !p.clear_init && p.changed == 0, "REFRESH when the game has not initialized it yet: nothing to clear");

    r = req(KILN_ITEMS_GEAR_AVAILABLE);
    r.available = 3;
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && !p.write_perks && p.changed == 0, "the same rarity: DONE, nothing written");

    r = req(KILN_ITEMS_GEAR_AVAILABLE);
    r.available = 1;
    p = plan(r, s, kInit, kEquipped);
    check(p.after.rarity == 0 && p.after.available == 1, "to Common: one slot");
}
}  // namespace

int main() {
    requests();
    rarity();
    plans();
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
