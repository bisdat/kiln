// SPDX-License-Identifier: MIT
// kiln.items v8 host test: gear writes without game memory (gear_logic.h): the request checks, the game's itemRarityOf,
// the type's last filled perk slot, and what a request plans for PerkContainerNew, Level, Durability and ItemState; the
// upgrades' request checks, cost split and commit check (upgrade_logic.h, package P3).
#include <cstdio>
#include <cstring>

#include <cmath>

#include "../gear_logic.h"
#include "../scale_logic.h"
#include "../upgrade_logic.h"

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
    x.reserved[1] = 1;  // package P7: the third reserved word became durability_max (checked in per_instance)
    check(!request_valid(x), "request: reserved[1] is refused");
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

// ---- upgrades (package P3): upgrade_logic.h ----
KilnItemsUpgradeRequest up(uint32_t flags) {
    KilnItemsUpgradeRequest u{};
    u.size = sizeof u;
    u.flags = flags;
    u.player = 3;
    u.gear.size = sizeof u.gear;
    u.gear.entity = 5000;
    u.gear.stamp = 7;
    if (flags & KILN_ITEMS_UPGRADE_WRITE) {
        u.gear.flags = KILN_ITEMS_GEAR_AVAILABLE | KILN_ITEMS_GEAR_DURABILITY_REFRESH;
        u.gear.available = 4;
    }
    u.cost_count = 1;
    u.costs[0] = {0xAAAA, 10};
    u.payer_count = 1;
    u.payers[0] = 3;
    return u;
}

void upgrades() {
    using namespace kitems::upgrade;
    check(sizeof(KilnItemsUpgradeCost) == 8 && sizeof(KilnItemsUpgradeRequest) == 160 && sizeof(KilnItemsUpgradeResult) == 392,
          "upgrade: the ABI sizes (8, 160, 392)");
    check(request_valid(up(KILN_ITEMS_UPGRADE_WRITE)), "upgrade: a write paid by the player");
    check(request_valid(up(0)), "upgrade: pay only (a failed roll that keeps the item)");
    KilnItemsUpgradeRequest u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.cost_count = 0;
    u.costs[0] = {};
    check(request_valid(u), "upgrade: free (no cost)");
    u.payer_count = 0;
    u.payers[0] = 0;
    check(request_valid(u), "upgrade: free needs no payer");
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.payer_count = 0;
    u.payers[0] = 0;
    check(!request_valid(u), "upgrade: a cost without a payer is refused");
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.player = 0;
    check(!request_valid(u), "upgrade: player 0 is refused");
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.gear.stamp = 0;
    check(!request_valid(u), "upgrade: an item without its stamp is refused");
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.gear.flags |= KILN_ITEMS_GEAR_DRY_RUN;
    check(!request_valid(u), "upgrade: the write's own DRY_RUN is refused (the upgrade's flag is the one)");
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.gear.available = 6;
    check(!request_valid(u), "upgrade: a write gear_write would refuse is refused");
    u = up(0);
    u.gear.available = 4;
    check(!request_valid(u), "upgrade: gear values without WRITE are refused");
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.flags |= 0x100;
    check(!request_valid(u), "upgrade: unknown flags are refused");
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.cost_count = 2;
    u.costs[1] = {0xAAAA, 3};
    check(!request_valid(u), "upgrade: a cost item named twice is refused");
    u.costs[1] = {0xBBBB, 0};
    check(!request_valid(u), "upgrade: a count of 0 is refused");
    u.costs[1] = {0xBBBB, KILN_ITEMS_UPGRADE_MAX_COUNT + 1};
    check(!request_valid(u), "upgrade: a count above 100000 is refused");
    u.costs[1] = {0xBBBB, KILN_ITEMS_UPGRADE_MAX_COUNT};
    check(request_valid(u), "upgrade: two cost items");
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.costs[3] = {0xCCCC, 1};
    check(!request_valid(u), "upgrade: an unused cost entry must be 0");
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.payer_count = 2;
    u.payers[1] = 3;
    check(!request_valid(u), "upgrade: a payer named twice is refused");
    u.payers[1] = 9000;
    check(request_valid(u), "upgrade: the player, then a chest");
    u.payers[5] = 9001;
    check(!request_valid(u), "upgrade: an unused payer entry must be 0");
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.cost_count = 9;
    check(!request_valid(u), "upgrade: more than 8 costs are refused");
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.size = sizeof u - 4;
    check(!request_valid(u), "upgrade: a short request is refused");
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    u.expect_available = 3;
    check(request_valid(u), "upgrade: expect_available 3");
    u.expect_available = 6;
    check(!request_valid(u), "upgrade: expect_available above 5 is refused");

    KilnItemsGearState now = rare_sword();
    u = up(KILN_ITEMS_UPGRADE_WRITE);
    check(as_expected(u, now), "expected: no check when expect_available is 0");
    u.expect_available = 3;
    check(as_expected(u, now), "expected: 3 slots as priced");
    u.expect_available = 4;
    check(!as_expected(u, now), "expected: priced at 4 slots, it has 3 (another upgrade came first): CHANGED");
    now.flags &= ~KILN_ITEMS_GEAR_HAS_PERKS;
    u.expect_available = 3;
    check(!as_expected(u, now), "expected: an item without slots never matches a priced count");

    const uint32_t have[3] = {4, 0, 9};
    Split s = split(10, have, 3);
    check(s.enough && s.take[0] == 4 && s.take[1] == 0 && s.take[2] == 6 && s.have == 13, "split: each payer in order pays what it has");
    s = split(4, have, 3);
    check(s.enough && s.take[0] == 4 && s.take[2] == 0, "split: the first payer alone when it has enough");
    s = split(14, have, 3);
    check(!s.enough && s.take[0] == 0 && s.take[2] == 0 && s.have == 13, "split: too little: nobody pays a part");
    s = split(1, have, 0);
    check(!s.enough && s.have == 0, "split: no payer: not enough");

    const KilnItemsUpgradeCost costs[2] = {{0xAAAA, 10}, {0xBBBB, 2}};
    const uint64_t before[2] = {13, 5}, ok_after[2] = {3, 3}, rolled[2] = {13, 5}, half[2] = {3, 5}, more[2] = {2, 3};
    check(commit_stood(before, ok_after, costs, 2), "commit: every cost came out in full");
    check(!commit_stood(before, rolled, costs, 2), "commit: nothing came out (it rolled back by itself): no write");
    check(!commit_stood(before, half, costs, 2), "commit: one cost missing: no write");
    check(!commit_stood(before, more, costs, 2), "commit: more came out than the cost: no write");

    check(plain_take(1, 0, 0), "op: a take that leaves the stack is a plain take");
    check(plain_take(1, 1, 0), "op: a take that empties a plain stack (+0x30 set, no item entity) is a plain take (lab dev29-eq-p3)");
    check(plain_take(1, 1, 0xFFFFFFFFu), "op: an emptied stack whose item entity reads 0xFFFFFFFF is plain");
    check(!plain_take(1, 1, 2613), "op: a take that empties a stack with an item entity (gear) is refused");
    check(!plain_take(0, 0, 0), "op: a put (kind 0) is never part of a payment");
}

// ---- package P7: levels outside the type's range, max durability, power and armour scales ----
void per_instance() {
    const KilnItemsGearState s = rare_sword();  // levels 5..55, durability 120 / 160
    KilnItemsGearRequest r = req(KILN_ITEMS_GEAR_LEVEL);
    r.level = 60;
    Plan p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_OUT_OF_RANGE && !p.write_level, "LEVEL 60 on a 5..55 type without ANY_LEVEL: OUT_OF_RANGE");
    r.flags |= KILN_ITEMS_GEAR_ANY_LEVEL;
    check(request_valid(r), "request: LEVEL with ANY_LEVEL");
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.write_level && p.after.level == 60 && p.after.level_max == 60 &&
              p.changed == KILN_ITEMS_GEAR_CHANGED_LEVEL,
          "LEVEL 60 with ANY_LEVEL: written, Level's max raised with it");
    r.level = 2;
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.after.level == 2 && p.after.level_max == 10, "LEVEL 2 (below the range) with ANY_LEVEL");
    KilnItemsGearState up = s;
    up.unlocked = 3;
    up.level = 13;
    up.level_max = 13;
    r.level = 3;
    p = plan(r, up, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_OUT_OF_RANGE, "ANY_LEVEL still needs a base of 1 or more (level 3 with 3 unlocked)");
    KilnItemsGearRequest x = req(KILN_ITEMS_GEAR_UNLOCKED | KILN_ITEMS_GEAR_ANY_LEVEL);
    x.unlocked = 1;
    check(!request_valid(x), "request: ANY_LEVEL without LEVEL is refused (it only modifies LEVEL)");

    r = req(KILN_ITEMS_GEAR_DURABILITY_MAX);
    r.durability_max = 320;
    check(request_valid(r), "request: DURABILITY_MAX alone is a request");
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.write_durability_max && p.write_durability && p.after.durability_max == 320 &&
              p.after.durability == 240 && !p.clear_init &&
              p.changed == (KILN_ITEMS_GEAR_CHANGED_DURABILITY_MAX | KILN_ITEMS_GEAR_CHANGED_DURABILITY),
          "DURABILITY_MAX 160 -> 320: current 120 -> 240 (the same share), the init bit kept");
    r.flags |= KILN_ITEMS_GEAR_DURABILITY_REPAIR;
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.after.durability == 320 && !p.clear_init, "DURABILITY_MAX with REPAIR: full at the new max");
    x = req(KILN_ITEMS_GEAR_DURABILITY_MAX | KILN_ITEMS_GEAR_DURABILITY_REFRESH);
    x.durability_max = 320;
    check(!request_valid(x), "request: DURABILITY_MAX with REFRESH is refused (the recompute would replace it)");
    x = req(KILN_ITEMS_GEAR_DURABILITY_MAX);
    x.durability_max = 0;
    check(!request_valid(x), "request: durability_max 0 is refused");
    x.durability_max = KILN_ITEMS_GEAR_MAX_DURABILITY + 1;
    check(!request_valid(x), "request: durability_max above the limit is refused");
    x = req(KILN_ITEMS_GEAR_LEVEL);
    x.level = 10;
    x.durability_max = 5;
    check(!request_valid(x), "request: durability_max without DURABILITY_MAX is refused");
    KilnItemsGearState fresh = s;
    fresh.state = 0;
    fresh.flags &= ~KILN_ITEMS_GEAR_DURABILITY_INIT;
    p = plan(r, fresh, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_REFUSED && !p.write_durability_max, "DURABILITY_MAX before the game computed durability: REFUSED");
    KilnItemsGearState nodur = s;
    nodur.flags &= ~KILN_ITEMS_GEAR_HAS_DURABILITY;
    p = plan(r, nodur, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_NOT_GEAR, "DURABILITY_MAX on an item without Durability: NOT_GEAR");
    KilnItemsGearState worn = s;
    worn.durability = 1;
    worn.durability_max = 1000;
    r = req(KILN_ITEMS_GEAR_DURABILITY_MAX);
    r.durability_max = 100;
    p = plan(r, worn, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.after.durability == 1, "a worn item stays usable: 1/1000 -> 1/100, not 0");
    worn.durability = 0;
    p = plan(r, worn, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.after.durability == 0 && !p.write_durability, "a broken item stays broken");
    r.durability_max = 160;
    r.flags |= KILN_ITEMS_GEAR_DRY_RUN;
    p = plan(r, s, kInit, kEquipped);
    check(p.status == KILN_ITEMS_GEAR_DONE && p.changed == 0 && !p.write_durability_max, "DURABILITY_MAX to the same max: nothing changes");

    // the scales' request checks and arithmetic
    KilnItemsGearScale g{};
    g.size = sizeof g;
    g.flags = KILN_ITEMS_SCALE_POWER;
    g.entity = 5000;
    g.stamp = 7;
    g.power = 1.5f;
    check(kitems::scale::request_valid(g), "scale: POWER 1.5");
    KilnItemsGearScale y = g;
    y.power = 0.0f;
    check(!kitems::scale::request_valid(y), "scale: power 0 is refused");
    y.power = 101.0f;
    check(!kitems::scale::request_valid(y), "scale: power above 100 is refused");
    y = g;
    y.flags = 0;
    check(!kitems::scale::request_valid(y), "scale: no flag is refused");
    y.flags = 4;
    check(!kitems::scale::request_valid(y), "scale: an unknown flag is refused");
    y = g;
    y.stamp = 0;
    check(!kitems::scale::request_valid(y), "scale: stamp 0 is refused");
    y = g;
    y.flags = KILN_ITEMS_SCALE_ARMOR;
    y.armor = std::nanf("");
    check(!kitems::scale::request_valid(y), "scale: an armour scale that is no number is refused");
    check(kitems::scale::apply(41, 1.5f) == 62 && kitems::scale::apply(41, 0.5f) == 21 && kitems::scale::apply(0, 3.0f) == 0,
          "scale: power 41 x 1.5 = 62 (61.5 rounded), x 0.5 = 21, 0 stays 0");
    int32_t values[9] = {10, 20, 30, 0, 0, 0, 0, 0, -4};
    const int32_t before[9] = {4, 20, 10, 0, 0, 0, 0, 0, -4};
    kitems::scale::apply_armor(values, before, 9, 2.0f);
    check(values[0] == 16 && values[1] == 20 && values[2] == 50 && values[8] == -4,
          "scale: armour x 2 doubles only what the piece added (6 -> 12 over 4, 20 -> 40 over 10)");

    // the table (one writer, lock-free readers)
    static kitems::scale::Table t;  // static: two 16384-slot arrays
    check(t.live() == 0 && t.power(5000) == 1.0f, "table: empty reads 1");
    check(t.set(5000, KILN_ITEMS_SCALE_POWER, 1.5f, 0.0f) && t.power(5000) == 1.5f && t.armor(5000) == 1.0f && t.live() == 1,
          "table: a power scale; the armour stays 1");
    check(t.set(5000, KILN_ITEMS_SCALE_ARMOR, 0.0f, 2.0f) && t.power(5000) == 1.5f && t.armor(5000) == 2.0f && t.live() == 1,
          "table: the armour of the same item; its power kept");
    check(t.set(5000, KILN_ITEMS_SCALE_POWER | KILN_ITEMS_SCALE_ARMOR, 1.0f, 1.0f) && t.power(5000) == 1.0f && t.live() == 0,
          "table: both back to 1 removes the item");
    check(t.set(5001, KILN_ITEMS_SCALE_POWER, 1.0f, 0.0f) && t.live() == 0 && t.used() == 1, "table: setting 1 on a new item adds nothing");
    bool all = true;
    for (uint32_t e = 10000; e < 10000 + kitems::scale::kCapacity; ++e) all = all && t.set(e, KILN_ITEMS_SCALE_POWER, 2.0f, 0.0f);
    check(all && t.live() == kitems::scale::kCapacity, "table: 8192 items fit");
    check(!t.set(9999, KILN_ITEMS_SCALE_POWER, 2.0f, 0.0f) && t.power(9999) == 1.0f, "table: the 8193rd item is refused, nothing changed");
    check(t.set(10000, KILN_ITEMS_SCALE_POWER, 3.0f, 0.0f) && t.power(10000) == 3.0f, "table: a full table still changes an item it has");
    for (uint32_t e = 10000; e < 10000 + 6000; ++e) t.set(e, KILN_ITEMS_SCALE_POWER, 1.0f, 0.0f);
    all = true;
    for (uint32_t e = 30000; e < 30000 + 6000; ++e) all = all && t.set(e, KILN_ITEMS_SCALE_POWER, 0.5f, 0.0f);
    bool kept = true;
    for (uint32_t e = 16000; e < 10000 + kitems::scale::kCapacity; ++e) kept = kept && t.power(e) == 2.0f;
    check(all && kept && t.live() == kitems::scale::kCapacity - 6000 + 6000 && t.power(30000) == 0.5f && t.power(10001) == 1.0f &&
              t.used() <= kitems::scale::kRebuildAt,
          "table: removing 6000 and adding 6000 more rebuilds past the dead slots; every live entry is kept");
    t.clear();
    check(t.live() == 0 && t.used() == 0 && t.power(30000) == 1.0f && t.power(16000) == 1.0f, "table: clear forgets every entry");
}

}  // namespace

int main() {
    requests();
    rarity();
    plans();
    upgrades();
    per_instance();
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
