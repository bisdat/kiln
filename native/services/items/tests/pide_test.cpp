// SPDX-License-Identifier: MIT
#define KILN_ITEMS_HOST_TEST
#include "../pide.cpp"

#include <cmath>
#include <cstdlib>
#include <thread>

namespace {
int failures = 0;
void check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    failures += !ok;
}
uint32_t level_index = 0;
struct Fixture {
    alignas(16) uint8_t frame[256]{};
    uint8_t info[0x800]{};
    uint8_t perks[2] = {4, 0};
    uint32_t level[4] = {0, 0, 30, 30};
    uint32_t item = 7;
    kitems::Override list[8]{};
    safetyhook::Context context{};
    Fixture() {
        context.rbp = reinterpret_cast<uintptr_t>(frame + 128);
        context.rdi = reinterpret_cast<uintptr_t>(info);
        list[0] = {kitems::kUsedItem, 0, &item, 4};
        list[1] = {kitems::kPerkContainerNew, 0, perks, 2};
        list[2] = {kitems::kLevel, 0, level, 16};
        *reinterpret_cast<kitems::Override**>(frame + 24) = list;
        *reinterpret_cast<uint64_t*>(frame + 32) = 3;
        *reinterpret_cast<uint64_t*>(frame + 40) = 8;
        info[0xcc] = kitems::kWeapons;
    }
    uint64_t count() const { return *reinterpret_cast<const uint64_t*>(frame + 32); }
};
void spawn(void* arg) { kitems::hk_spawn(static_cast<Fixture*>(arg)->context); }
void fail_commit(void* arg) { spawn(arg); throw 1; }
}

int main() {
    using namespace kitems;
    static_assert(sizeof(KilnItemsRequest) == 40 && offsetof(KilnItemsRequest, unlock) == 25 &&
                  offsetof(KilnItemsRequest, pide) == 28 && offsetof(KilnItemsRequest, slot) == 32);
    check(pide_request_valid(0, 0), "legacy requests keep the same layout and zero reserved bytes");
    check(layout_ok(0x98, 0xa0, 0xcc) && !layout_ok(0x198, 0x1a0, 0xcc) && !layout_ok(0x98, 0xa1, 0xcc),
          "layout validation sign-extends real d8 captures without accepting truncated or mismatched offsets");
    bool bounds = true;
    for (int i = 0; i <= 255; ++i)
        bounds &= pide_request_valid(KILN_ITEMS_UNLOCK, static_cast<uint8_t>(i)) == ((i >= 1 && i <= 5) || i == 255);
    check(bounds && !pide_request_valid(0, 1), "native unlock accepts exactly 1..5 or 255 with its flag");
    check(!pide_request_valid(KILN_ITEMS_RECIPE | KILN_ITEMS_UNLOCK, 1) &&
          !pide_request_valid(KILN_ITEMS_RECIPE | KILN_ITEMS_GEM_SLOT, 0), "recipe results cannot acquire give-only options");
    g_ok = true;
    g_list = -104; g_count = -96; g_category = 0xcc; g_level_index = &level_index;

    Fixture plain;
    spawn(&plain);
    check(plain.perks[1] == 0 && plain.level[2] == 30 && plain.count() == 3, "ordinary loot and crafting spawns are unchanged");
    Fixture gear;
    pide_commit(&gear, &spawn, KILN_ITEMS_UNLOCK | KILN_ITEMS_GEM_SLOT, 255, 7);
    check(gear.perks[1] == 4 && gear.level[2] == 34 && gear.level[3] == 30 && gear.count() == 4 &&
          gear.list[3].hash == kWeaponGemSlot && gear.list[3].size == 12,
          "a give unlocks only available perks, raises the current level and adds an empty weapon gem slot");
    spawn(&plain);
    check(plain.perks[1] == 0 && plain.count() == 3, "successful commit clears the options before the next unrelated spawn");
    Fixture failed;
    try { pide_commit(&failed, &fail_commit, KILN_ITEMS_UNLOCK | KILN_ITEMS_GEM_SLOT, 2, 7); } catch (...) {}
    spawn(&plain);
    check(!t_flags && !t_unlock && !t_item && plain.perks[1] == 0 && plain.count() == 3,
          "a faulted commit clears the options immediately, without waiting for another give");
    Fixture unrelated;
    pide_commit(&unrelated, &spawn, KILN_ITEMS_UNLOCK | KILN_ITEMS_GEM_SLOT, 2, 99);
    check(unrelated.perks[1] == 0 && unrelated.count() == 3, "a different item created during a give cannot inherit its options");
    Fixture armour;
    armour.info[0xcc] = 1;
    pide_commit(&armour, &spawn, KILN_ITEMS_UNLOCK | KILN_ITEMS_GEM_SLOT, 2, 7);
    check(armour.perks[1] == 2 && armour.level[2] == 32 && armour.count() == 3, "armour can unlock perks but never gets a weapon gem slot");
    Fixture full;
    *reinterpret_cast<uint64_t*>(full.frame + 40) = 3;
    pide_commit(&full, &spawn, KILN_ITEMS_GEM_SLOT, 0, 7);
    check(full.count() == 3, "a full override list is never overrun");
    Fixture gem;
    gem.list[3] = {kWeaponGemSlot, 0, g_zero_gem, 12};
    *reinterpret_cast<uint64_t*>(gem.frame + 32) = 4;
    pide_commit(&gem, &spawn, KILN_ITEMS_GEM_SLOT, 0, 7);
    check(gem.count() == 4, "a weapon that already rolled a gem slot never gets a second one");
    Fixture missing_level;
    missing_level.list[2].size = 0;
    pide_commit(&missing_level, &spawn, KILN_ITEMS_UNLOCK, 2, 7);
    check(missing_level.perks[1] == 0, "a missing level override cannot leave perks unlocked without their level increments");
    level_index = 2;
    Fixture bad_index;
    pide_commit(&bad_index, &spawn, KILN_ITEMS_UNLOCK, 2, 7);
    check(bad_index.perks[1] == 0 && bad_index.level[2] == 30, "an unknown level index fails closed without a partial upgrade");
    level_index = 0;
    bool isolated = false;
    pide_commit(&isolated, [](void* arg) {
        std::thread worker([arg] {
            Fixture other;
            spawn(&other);
            *static_cast<bool*>(arg) = other.perks[1] == 0 && other.count() == 3;
        });
        worker.join();
        Fixture nested;
        pide_commit(&nested, &spawn, 0, 0, 7);
        *static_cast<bool*>(arg) &= nested.perks[1] == 0 && t_flags == KILN_ITEMS_UNLOCK && t_unlock == 2;
    }, KILN_ITEMS_UNLOCK, 2, 7);
    check(isolated && !t_flags, "give options are thread local and nested plain commits restore the outer scope");

    // ---- v7, the gem wave: sockets on any gear ----
    check(socket_kind(2, 1) == kKindWeapon && socket_kind(1, 37) == kKindRings && socket_kind(1, 13) == kKindArmour &&
          socket_kind(1, 17) == kKindArmour && socket_kind(1, 18) == kKindShields && socket_kind(1, 38) == kKindOther &&
          socket_kind(3, 2) == kKindOther && socket_kind(15, 0) == kKindNone && socket_kind(16, 0) == kKindNone &&
          socket_kind(0, 19) == kKindNone && socket_kind(1, 0xFF) == kKindNone,
          "kinds: rings 37, armour 13-17, shields 18 of Equipment; gems, dyes and customization never; unknown layout no kind");
    check(!pide_request_valid(KILN_ITEMS_GEM_SLOT_ANY, 0) && pide_request_valid(KILN_ITEMS_GEM_SLOT | KILN_ITEMS_GEM_SLOT_ANY, 0) &&
          !pide_request_valid(KILN_ITEMS_RECIPE | KILN_ITEMS_DYE, 0) && !pide_request_valid(KILN_ITEMS_RECIPE | KILN_ITEMS_DURABILITY, 0),
          "GEM_SLOT_ANY needs GEM_SLOT; dye and durability are give-only like the other gear options");
    auto ring = [](Fixture& f) { f.info[0xcc] = 1; f.info[0x104] = 37; };
    auto has_gem = [](const Fixture& f) {
        for (uint64_t i = 0; i < f.count(); ++i) if (f.list[i].hash == kWeaponGemSlot && f.list[i].size == 12) return true;
        return false;
    };
    // the feature off (no gemSlotOverride offset): v7 behaviour is off, rolls are refused
    g_override = 0;
    Fixture off_ring; ring(off_ring);
    pide_commit(&off_ring, &spawn, KILN_ITEMS_GEM_SLOT | KILN_ITEMS_GEM_SLOT_ANY, 0, 7);
    KilnItemsSocketRoll roll{sizeof(KilnItemsSocketRoll), 1.0f, 0, 0, 0, 0};
    check(!has_gem(off_ring) && pide_socket_roll(1, &roll) == KILN_ERR_UNAVAILABLE && pide_socket_roll(1, nullptr) == KILN_OK,
          "without feature gems.sockets a ring gets no socket and rolls are unavailable (ending them still works)");
    g_override = 0x798;
    Fixture any_ring; ring(any_ring);
    pide_commit(&any_ring, &spawn, KILN_ITEMS_GEM_SLOT | KILN_ITEMS_GEM_SLOT_ANY, 0, 7);
    Fixture plain_ring; ring(plain_ring);
    pide_commit(&plain_ring, &spawn, KILN_ITEMS_GEM_SLOT, 0, 7);
    check(has_gem(any_ring) && any_ring.count() == 4 && !has_gem(plain_ring) && plain_ring.count() == 3,
          "GEM_SLOT_ANY gives a ring an empty gem slot; GEM_SLOT alone stays weapons-only");
    Fixture gem_item; gem_item.info[0xcc] = 15;
    pide_commit(&gem_item, &spawn, KILN_ITEMS_GEM_SLOT | KILN_ITEMS_GEM_SLOT_ANY, 0, 7);
    Fixture dye_item; dye_item.info[0xcc] = 16;
    pide_commit(&dye_item, &spawn, KILN_ITEMS_GEM_SLOT | KILN_ITEMS_GEM_SLOT_ANY, 0, 7);
    check(!has_gem(gem_item) && !has_gem(dye_item), "a gem or a dye never gets a gem slot, even with GEM_SLOT_ANY");
    Fixture any_other_item; ring(any_other_item);
    pide_commit(&any_other_item, &spawn, KILN_ITEMS_GEM_SLOT | KILN_ITEMS_GEM_SLOT_ANY, 0, 99);
    check(!has_gem(any_other_item), "GEM_SLOT_ANY is scoped to the give's own item");
    Fixture weapon_any;
    pide_commit(&weapon_any, &spawn, KILN_ITEMS_GEM_SLOT | KILN_ITEMS_GEM_SLOT_ANY, 0, 7);
    int weapon_slots = 0;
    for (uint64_t i = 0; i < weapon_any.count(); ++i) weapon_slots += weapon_any.list[i].hash == kWeaponGemSlot;
    check(weapon_slots == 1, "a weapon with GEM_SLOT_ANY gets exactly one slot (the weapons path)");
    // gemSlotOverride.isSet on a non-weapon: honoured on every creation, without a give
    Fixture override_ring; ring(override_ring); override_ring.info[0x798] = 1;
    spawn(&override_ring);
    Fixture override_weapon; override_weapon.info[0x798] = 1;
    spawn(&override_weapon);
    Fixture override_off; ring(override_off);
    spawn(&override_off);
    check(has_gem(override_ring) && !has_gem(override_weapon) && !has_gem(override_off),
          "gemSlotOverride gives a looted ring its slot; weapons stay with the game's own roll; vanilla rings unchanged");
    // socket rolls
    static float fixed = 0.3f;
    g_rand = [] { return fixed; };
    KilnItemsSocketRoll half{sizeof(KilnItemsSocketRoll), 0.5f, 0.2f, 0, 0, 0};
    check(pide_socket_roll(1, &half) == KILN_OK, "a valid roll is accepted");
    Fixture rolled_ring; ring(rolled_ring);
    spawn(&rolled_ring);
    Fixture rolled_chest; rolled_chest.info[0xcc] = 1; rolled_chest.info[0x104] = 14;
    spawn(&rolled_chest);
    Fixture rolled_shield; rolled_shield.info[0xcc] = 1; rolled_shield.info[0x104] = 18;
    spawn(&rolled_shield);
    Fixture rolled_weapon;
    spawn(&rolled_weapon);
    check(has_gem(rolled_ring) && !has_gem(rolled_chest) && !has_gem(rolled_shield) && !has_gem(rolled_weapon),
          "a roll of 0.3 against rings 0.5 / armour 0.2 / shields 0: only the ring; weapons never roll here");
    KilnItemsSocketRoll second{sizeof(KilnItemsSocketRoll), 0.1f, 0.9f, 0, 0, 0};
    check(pide_socket_roll(2, &second) == KILN_OK, "a second owner's roll is accepted");
    Fixture max_ring; ring(max_ring);
    spawn(&max_ring);
    Fixture max_chest; max_chest.info[0xcc] = 1; max_chest.info[0x104] = 14;
    spawn(&max_chest);
    check(has_gem(max_ring) && has_gem(max_chest), "owners combine: each kind rolls with the highest chance asked");
    pide_socket_roll(1, nullptr);
    pide_socket_roll(2, nullptr);
    Fixture ended; ring(ended);
    spawn(&ended);
    check(!has_gem(ended) && !g_roll_any.load(), "ending every owner's rolls stops them");
    KilnItemsSocketRoll bad = half;
    bad.rings = 1.5f;
    KilnItemsSocketRoll nan = half;
    nan.armour = std::nanf("");
    KilnItemsSocketRoll small = half;
    small.size = 8;
    KilnItemsSocketRoll reserved = half;
    reserved.reserved = 1;
    check(pide_socket_roll(1, &bad) == KILN_ERR_INVALID_ARGUMENT && pide_socket_roll(1, &nan) == KILN_ERR_INVALID_ARGUMENT &&
          pide_socket_roll(1, &small) == KILN_ERR_INVALID_ARGUMENT && pide_socket_roll(1, &reserved) == KILN_ERR_INVALID_ARGUMENT &&
          !g_roll_any.load(), "chances outside 0..1, NaN, a short struct or reserved bits are refused and change nothing");
    g_rand = &rand01;
    // dye and durability of a re-created item (Gems.AddSocket)
    PideOptions extra;
    extra.flags = KILN_ITEMS_GEM_SLOT | KILN_ITEMS_GEM_SLOT_ANY | KILN_ITEMS_DYE | KILN_ITEMS_DURABILITY;
    extra.item = 7;
    extra.dye = 0xabcdef01u;
    extra.durability = 0.25f;
    Fixture dyed; ring(dyed);
    float seen_durability = -1;
    uint32_t seen_dye = 0;
    struct Probe { Fixture* f; float* d; uint32_t* y; } probe{&dyed, &seen_durability, &seen_dye};
    pide_commit(&probe, [](void* arg) {
        auto* p = static_cast<Probe*>(arg);
        spawn(p->f);
        for (uint64_t i = 0; i < p->f->count(); ++i) {  // read while the scope (and its data) lives, as the spawn does
            if (p->f->list[i].hash == kDurabilityInit) std::memcpy(p->d, p->f->list[i].data, 4);
            if (p->f->list[i].hash == kDye) std::memcpy(p->y, p->f->list[i].data, 4);
        }
    }, extra);
    check(has_gem(dyed) && dyed.count() == 6 && seen_durability == 0.25f && seen_dye == 0xabcdef01u,
          "a re-created ring gets its slot, then DurabilityPercentageInit 0.25 and its dye appended");
    Fixture has_dye; ring(has_dye);
    uint32_t game_dye = 5;
    has_dye.list[3] = {kDye, 0, &game_dye, 4};
    *reinterpret_cast<uint64_t*>(has_dye.frame + 32) = 4;
    PideOptions only_dye;
    only_dye.flags = KILN_ITEMS_DYE;
    only_dye.item = 7;
    only_dye.dye = 9;
    pide_commit(&has_dye, &spawn, only_dye);
    check(game_dye == 9 && has_dye.count() == 4, "a list that already has a dye entry gets its value replaced, not a second entry");
    Fixture full_extra; ring(full_extra);
    *reinterpret_cast<uint64_t*>(full_extra.frame + 40) = 5;  // room for two more entries
    pide_commit(&full_extra, &spawn, extra);
    bool dur_in = false, dye_in = false;
    for (uint64_t i = 0; i < full_extra.count(); ++i) {
        dur_in |= full_extra.list[i].hash == kDurabilityInit;
        dye_in |= full_extra.list[i].hash == kDye;
    }
    check(has_gem(full_extra) && dur_in && !dye_in && full_extra.count() == 5,
          "with room for two, the gem slot and the durability win over the dye; the list is never overrun");
    check(!t_flags && !t_dye && t_durability == 1.0f, "the extras' scope ends with the commit");
    uint64_t by_give = 0, by_override = 0, by_roll = 0, extras = 0;
    pide_socket_counters(&by_give, &by_override, &by_roll, &extras);
    check(by_give == 3 && by_override == 1 && by_roll == 3 && extras == 4,
          "the counters say how each socket came about (gives 3, overrides 1, rolls 3) and count 4 extras");

    std::printf("pide options: %d failure(s)\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
