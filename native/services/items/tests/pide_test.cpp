// SPDX-License-Identifier: MIT
#define KILN_ITEMS_HOST_TEST
#include "../pide.cpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
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
    Fixture equipped;
    uint8_t initial_state[20]{}; initial_state[0x0e]=0x40;
    equipped.list[3]={0x3a2fac75u,0,initial_state,20};
    *reinterpret_cast<uint64_t*>(equipped.frame+32)=4;
    PideOptions equip_options; equip_options.item=7; equip_options.equipped=true;
    pide_commit(&equipped,&spawn,equip_options);
    check(initial_state[0x0e]==0x48 && !t_equipped, "GiveEquip: initial ItemState is equipped before spawn; the scope ends at commit");
    initial_state[0x0e]=0x40; spawn(&equipped);
    check(initial_state[0x0e]==0x40, "GiveEquip: ordinary later creation does not inherit IsEquipped");
    equip_options.item=99; pide_commit(&equipped,&spawn,equip_options);
    check(initial_state[0x0e]==0x40, "GiveEquip: another item cannot inherit IsEquipped");
    equip_options.item=7;
    try { pide_commit(&equipped,&fail_commit,equip_options); } catch (...) {}
    check(!t_equipped, "GiveEquip: a fault clears the initial-equipped scope");
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
    // review: availability is told before the arguments, so Kiln.Items' probe (an invalid roll) never ends anyone's rolls
    KilnItemsSocketRoll invalid_probe{};  // size 0: never valid
    g_override = 0;
    const KilnResult off_probe = pide_socket_roll(0, &invalid_probe);
    g_override = 0x798;
    KilnItemsSocketRoll core_roll{sizeof(KilnItemsSocketRoll), 0.4f, 0, 0, 0, 0};
    const KilnResult core_set = pide_socket_roll(0, &core_roll);
    const KilnResult on_probe = pide_socket_roll(0, &invalid_probe);
    check(off_probe == KILN_ERR_UNAVAILABLE && core_set == KILN_OK && on_probe == KILN_ERR_INVALID_ARGUMENT &&
              g_roll_any.load() && roll_chance(kKindRings) == 0.4f,
          "without gems.sockets even an invalid roll says unavailable; with it the probe is refused and the core's own rolls stay");
    pide_socket_roll(0, nullptr);
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
    uint32_t seen_existing = 0;
    struct DyeProbe { Fixture* f; uint32_t* y; } dye_probe{&has_dye, &seen_existing};
    pide_commit(&dye_probe, [](void* arg) {
        auto* p = static_cast<DyeProbe*>(arg);
        spawn(p->f);
        for (uint64_t i = 0; i < p->f->count(); ++i)
            if (p->f->list[i].hash == kDye) std::memcpy(p->y, p->f->list[i].data, 4);
    }, only_dye);
    // review: the entry's data may be the game's (an ItemInfo default on another build); it is pointed at the give's
    // value, never written through
    check(game_dye == 5 && seen_existing == 9 && has_dye.count() == 4,
          "a list that already has a dye entry spawns the give's dye without a second entry and without writing the game's buffer");
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


    // ---- v8 (package P5): rarity rolls at creation ----
    check(rolls::source_of(false, false) == KILN_ITEMS_ROLL_CRAFT && rolls::source_of(false, true) == KILN_ITEMS_ROLL_LOOT &&
              rolls::source_of(true, false) == KILN_ITEMS_ROLL_GIVE && rolls::source_of(true, true) == 0,
          "sources: no rarity asked = craft, a rarity asked = loot, a give of this item = give, a give with a rarity is never rolled");
    {
        const float w[5] = {50, 30, 15, 4, 1};
        int n[5] = {};
        for (int i = 0; i < 1000; ++i) ++n[rolls::pick(w, 0x1f, (i + 0.5f) / 1000)];
        check(n[0] == 500 && n[1] == 300 && n[2] == 150 && n[3] == 40 && n[4] == 10,
              "weights 50/30/15/4/1 over an even spread of 1000 draws give 500/300/150/40/10");
        const float legendary[5] = {0, 0, 0, 0, 1};
        const float ends[5] = {1, 0, 0, 0, 1};
        check(rolls::pick(legendary, 0x0f, 0.5f) == -1 && rolls::pick(legendary, 0x1f, 0.999f) == 4 &&
                  rolls::pick(ends, 0x0f, 0.999f) == 0,
              "a rarity whose perk slot the type lacks never comes up; with none left the pick says so");
    }
    struct RollFixture {
        uint8_t info[0x800]{};
        safetyhook::Context c{};
        RollFixture(uint32_t item, uint16_t asked) {
            std::memcpy(info, &item, 4);
            for (int r = 0; r < 5; ++r) info[0x608 + 16 * r + 3] = 0x5a;  // perkReferences[r] set
            c.rdi = reinterpret_cast<uintptr_t>(info);
            c.rbx = 0xabcdef0000000000ull | asked;  // ebx = the word createPide just loaded: {value, has}
        }
        uint32_t word() const { return static_cast<uint32_t>(c.rbx & 0xffff); }
        bool high_kept() const { return (c.rbx >> 16) == 0xabcdef000000ull; }
    };
    auto create = [](uint32_t item, uint16_t asked) {
        RollFixture f(item, asked);
        hk_rarity(f.c);
        return f;
    };
    g_perk_refs = 0;
    g_rolls_hooked = false;
    KilnItemsRarityRoll epic{0x1234, KILN_ITEMS_ROLL_CRAFT, {0, 0, 0, 1, 0}, 0};
    check(pide_rarity_rolls(1, &epic, 1, sizeof epic) == KILN_ERR_UNAVAILABLE && pide_rarity_rolls(1, nullptr, 0, 0) == KILN_OK &&
              create(0x1234, 0).word() == 0,
          "without the hook (feature items.gear.rolls off) a set is unavailable, ending one works, and nothing is rolled");
    g_rolls_hooked = true;
    g_resolve_perk_refs = [](uint32_t* offset, char*, size_t) { *offset = 0x608; return true; };  // as kiln.types answers
    check(create(0x1234, 0).word() == 0, "no rules: a craft keeps the game's rarity");
    check(pide_rarity_rolls(1, &epic, 1, sizeof epic) == KILN_OK && g_perk_refs == 0x608, "a valid set is taken; the perk slots' offset is resolved at the first set");
    RollFixture crafted = create(0x1234, 0);
    check(crafted.word() == 0x0103 && crafted.high_kept(), "a craft of a named item comes out Epic: the asked word becomes {3, has 1}, the rest of rbx kept");
    check(create(0x1234, 0x0101).word() == 0x0101, "a loot creation (a rarity asked) is left alone by a craft-only rule");
    check(create(0x9999, 0).word() == 0, "an item type no rule names is never touched");
    t_item = 0x1234;
    const uint32_t plain_give = create(0x1234, 0).word(), rarity_give = create(0x1234, 0x0102).word();
    t_item = 0;
    check(plain_give == 0 && rarity_give == 0x0102, "a kiln.items give is no craft: a plain give is left alone without GIVE, a give with a rarity keeps it");
    KilnItemsRarityRoll all{0x1234, KILN_ITEMS_ROLL_CRAFT | KILN_ITEMS_ROLL_LOOT | KILN_ITEMS_ROLL_GIVE, {0, 0, 0, 0, 1}, 0};
    check(pide_rarity_rolls(1, &all, 1, sizeof all) == KILN_OK, "a later set of the same owner replaces its rules");
    t_item = 0x1234;
    const uint32_t give_rolled = create(0x1234, 0).word(), give_kept = create(0x1234, 0x0102).word();
    t_item = 0;
    check(create(0x1234, 0x0101).word() == 0x0104 && give_rolled == 0x0104 && give_kept == 0x0102,
          "with LOOT and GIVE: loot re-rolls to Legendary, a plain give rolls, a give with a rarity still keeps it");
    {
        RollFixture noslot(0x1234, 0);
        noslot.info[0x608 + 16 * 4 + 3] = 0;
        hk_rarity(noslot.c);
        check(noslot.word() == 0, "a Legendary-only rule on a type without perk slot 4 leaves the game's rarity (no empty slot)");
    }
    KilnItemsRarityRoll rare{0x1234, KILN_ITEMS_ROLL_CRAFT, {0, 0, 1, 0, 0}, 0};
    check(pide_rarity_rolls(2, &rare, 1, sizeof rare) == KILN_OK && create(0x1234, 0).word() == 0x0102,
          "owners merge: the owner that set its rules last wins an item both name");
    check(pide_rarity_rolls(2, nullptr, 0, 0) == KILN_OK && create(0x1234, 0).word() == 0x0104, "ending owner 2's set gives owner 1's rule back");
    {
        KilnItemsRarityRoll two[2] = {all, all};
        KilnItemsRarityRoll nan = all, zero = all, nosrc = all, badsrc = all, noitem = all, reserved = all, negative = all;
        nan.weights[1] = std::nanf("");
        for (float& f : zero.weights) f = 0;
        nosrc.sources = 0;
        badsrc.sources = 8;
        noitem.item = 0;
        reserved.reserved = 1;
        negative.weights[0] = -1;
        const bool refused = pide_rarity_rolls(3, two, 2, sizeof all) == KILN_ERR_INVALID_ARGUMENT &&
                             pide_rarity_rolls(3, &nan, 1, sizeof all) == KILN_ERR_INVALID_ARGUMENT &&
                             pide_rarity_rolls(3, &zero, 1, sizeof all) == KILN_ERR_INVALID_ARGUMENT &&
                             pide_rarity_rolls(3, &nosrc, 1, sizeof all) == KILN_ERR_INVALID_ARGUMENT &&
                             pide_rarity_rolls(3, &badsrc, 1, sizeof all) == KILN_ERR_INVALID_ARGUMENT &&
                             pide_rarity_rolls(3, &noitem, 1, sizeof all) == KILN_ERR_INVALID_ARGUMENT &&
                             pide_rarity_rolls(3, &reserved, 1, sizeof all) == KILN_ERR_INVALID_ARGUMENT &&
                             pide_rarity_rolls(3, &negative, 1, sizeof all) == KILN_ERR_INVALID_ARGUMENT &&
                             pide_rarity_rolls(3, &all, 1, sizeof all - 4) == KILN_ERR_INVALID_ARGUMENT &&
                             pide_rarity_rolls(3, &all, rolls::kMaxRules + 1, sizeof all) == KILN_ERR_INVALID_ARGUMENT;
        check(refused && create(0x1234, 0).word() == 0x0104,
              "an item twice, NaN, no weight above 0, no or an unknown source, item 0, reserved bits, a negative weight, a short entry or too many: refused, nothing changes");
    }
    {
        struct Wide { KilnItemsRarityRoll r; uint32_t newer; } wide[2] = {{rare, 7}, {{0x4321, KILN_ITEMS_ROLL_CRAFT, {1, 0, 0, 0, 0}, 0}, 7}};
        wide[0].r.item = 0x5555;
        check(pide_rarity_rolls(4, &wide[0].r, 2, sizeof(Wide)) == KILN_OK && create(0x5555, 0).word() == 0x0102 && create(0x4321, 0).word() == 0x0100,
              "entries are read at the caller's stride (a newer, larger entry)");
        pide_rarity_rolls(4, nullptr, 0, 0);
    }
    {
        RollFixture faulty(0x1234, 0);
        faulty.c.rdi = 0;
        hk_rarity(faulty.c);
        check(faulty.word() == 0, "no ItemInfo: left as the game made it");
    }
    KilnItemsRollStatus st{};
    st.size = sizeof st;
    check(pide_roll_status(&st) == KILN_OK && st.available == 1 && st.items == 1 && st.owners == 1 && st.rolled[3] == 1 &&
              st.rolled[4] == 4 && st.rolled[2] == 2 && st.rolled[0] == 1 && st.kept >= 3 && st.no_slot == 1,
          "the status counts rules, owners and every rarity rolled, kept and no-slot creation");
    pide_rarity_rolls(1, nullptr, 0, 0);
    check(create(0x1234, 0).word() == 0 && !g_published.load(), "ending the last set stops every roll");

    std::printf("pide options: %d failure(s)\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
