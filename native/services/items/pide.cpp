// SPDX-License-Identifier: MIT
// kiln.items: gear options at creation (KILN_ITEMS_UNLOCK, KILN_ITEMS_GEM_SLOT; docs/re/items.md section 10,
// docs/re/creative-mode-146.md Part 3 section 2). createPide builds a list of component overrides
// {u32 component hash; void* data; u64 size} and spawns the item's pide template with it. A mid hook right before it
// copies the list (items/PideSpawn) changes it, only while a give of this service that asked for an option commits on
// this thread (pide_scope):
//  - UNLOCK: PerkContainerNew {availablePerks, unlockedPerks}: unlocked = min(available, asked); the Level override's
//    current value += the newly unlocked count (the workbench's upgrade does both, one at a time; docs/re/items.md).
//  - GEM_SLOT: on a Weapon (ItemInfo category 2) whose list has no WeaponGemSlot (the game's roll did not give one),
//    one entry {WeaponGemSlot, 12 zero bytes = an empty ItemStack} is appended while the list has room.
// v7, the gem wave (feature gems.sockets: sigdb/symbols/server.d/gems-sockets.json; docs/kiln/services/gems-sockets.md).
// The game's own gem slot is weapons-only: category 2 and (the BalancingRegistry chance roll or ItemInfo.gemSlotOverride),
// server 0x1b1cf5..0x1b1e14. The rest of the gem system is category-blind, so the hook widens creation:
//  - GEM_SLOT_ANY (with GEM_SLOT, scoped to a give): any gear but gems, dyes and customization items gets the slot;
//  - gemSlotOverride.isSet on a non-weapon (no game item sets it outside the weapons; a data pack does): the slot, on
//    every creation (loot, crafting, gives), as the game does for weapons;
//  - socket rolls (socket_roll): a non-weapon of a kind with a chance rolls the slot on every creation;
//  - DYE / DURABILITY (scoped to a give, used by Gems.AddSocket's re-creation): a ColorPaletteItem {dye} and a
//    DurabilityPercentageInit {fraction} entry (the game's item_durability_init reads the latter), replaced when the
//    list has one, else appended.
// Weapons keep the game's own roll; an item whose list already has a WeaponGemSlot never gets a second one.
// v8, the equipment wave (package P5, feature items.gear.rolls): rarity rolls at creation. A second mid hook, where
// createPide loads the rarity its caller asked for (`movzx ebx, word [opts+8]`: OptU8 {value, has}; items/PideRarity is
// the instruction after it), replaces that word for an item type a rule names with {rolled rarity, has 1}, chosen by
// weight among the rarities the type has a perk slot for (ItemInfo.perkReferences[r], the slot createPide itself tests).
// The game then makes the item at that rarity: PerkContainerNew, the weapon gem slot's chance, durability. Which
// creations a rule takes (rolls_logic.h source_of): a give of this service for this item type (t_item, the give scope
// below) with or without a rarity, or another creation with (loot) or without (crafting) one. A give that asked for a
// rarity is never rolled. Nothing is written to the game's memory: only the register.
// Component hashes are FNV-1a-32 of the reflected type names, so they hold across builds. Faults count; the item is
// then created as the game made it.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "kiln_hook.hpp"
#include "pide.h"
#include "rolls_logic.h"
#include "safetyhook.hpp"  // Context only: the mid handler keeps its safetyhook::Context& signature
#ifndef KILN_ITEMS_HOST_TEST
#include "services/kiln_types.h"
#endif

namespace kitems {
namespace {

constexpr uint32_t kPerkContainerNew = 0xe288d50cu;  // keen::ecs::PerkContainerNew {u8 available, u8 unlocked}
constexpr uint32_t kLevel = 0xe550da37u;             // keen::ecs::Level: 8-byte attribute header, u32 values[2]
constexpr uint32_t kWeaponGemSlot = 0x84ba9b52u;     // keen::ecs::WeaponGemSlot {ItemStack weaponGem} (12 bytes)
constexpr uint32_t kUsedItem = 0x0b02361du;          // keen::ecs::UsedItem {u32 itemId}
constexpr uint32_t kDye = 0x103e50eau;               // keen::ecs::ColorPaletteItem {u32 paletteItemId} (4 bytes)
constexpr uint32_t kDurabilityInit = 0x41184ffau;    // keen::ecs::DurabilityPercentageInit {float value} (4 bytes)
constexpr uint8_t kEquipment = 1;                    // ItemCategory.Equipment (rings, armour, shields, backpacks)
constexpr uint8_t kWeapons = 2;                      // ItemCategory.Weapons
constexpr uint32_t kMaxPerks = 5;
// ItemInfo.equipment.slot (EquipmentSlotType) sits 0x38 after ItemInfo.category (0xcc / 0x104, kfc's reflection of
// keen::ItemInfo; the catalog reads both at these offsets). Only used when the category constant is 0xcc.
constexpr int64_t kCategoryAtFixedLayout = 0xcc;
constexpr int64_t kEquipSlotAfterCategory = 0x104 - 0xcc;

struct Override {
    uint32_t hash;
    uint32_t pad;
    void* data;
    uint64_t size;
};
static_assert(sizeof(Override) == 24);

alignas(16) uint8_t g_zero_gem[16] = {};  // the appended gem slot's data (read by the spawn, never written)
kiln::MidHook2 h_spawn;
int64_t g_list = 0, g_count = 0, g_category = 0;  // rbp displacements (int8), ItemInfo offset
int64_t g_override = 0;                          // v7: ItemInfo.gemSlotOverride.isSet (0 = feature gems.sockets off)
const uint32_t* g_level_index = nullptr;         // createPide's current-level index into Level's values
bool g_ok = false;

thread_local bool t_equipped = false;
thread_local uint32_t t_flags = 0;
thread_local uint8_t t_unlock = 0;
thread_local uint32_t t_item = 0;
thread_local uint32_t t_dye = 0;      // the appended ColorPaletteItem's data (lives through the commit)
thread_local float t_durability = 1;  // the appended DurabilityPercentageInit's data

// v7 socket rolls: per owner, and the highest chance of each kind (float bits; read lock-free by the hook)
std::mutex g_roll_mu;
std::unordered_map<KilnOwner, KilnItemsSocketRoll> g_rolls;
std::atomic<uint32_t> g_roll_bits[4] = {};
std::atomic<bool> g_roll_any{false};

float rand01() {  // per thread: xorshift64*, seeded from the thread and the clock
    thread_local uint64_t x = 0;
    if (x == 0) {
        x = (static_cast<uint64_t>(GetCurrentThreadId()) << 32) ^ GetTickCount64() ^ reinterpret_cast<uintptr_t>(&x);
        if (x == 0) x = 0x9e3779b97f4a7c15ull;
    }
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return static_cast<float>((x * 0x2545f4914f6cdd1dull) >> 40) / static_cast<float>(1u << 24);  // [0, 1)
}
float (*g_rand)() = &rand01;  // tests replace it

struct Counters {
    std::atomic<uint64_t> unlocked{0}, gems{0}, faults{0};
    std::atomic<uint64_t> any{0}, overrides{0}, rolled{0}, extras{0};  // v7
} C;

// v8 (P5): rarity rolls. The merged rules are published as an immutable sorted vector the hook reads without a lock;
// replaced vectors stay alive for the process (rules change at mod loads, not per item).
kiln::MidHook2 h_rarity;
bool g_rolls_hooked = false;  // items/PideRarity hooked (feature items.gear.rolls)
uint32_t g_perk_refs = 0;     // ItemInfo.perkReferences (5 x 16 bytes), by reflection; 0 = not resolved yet
std::mutex g_rules_mu;
rolls::Table g_rules;  // every owner's set (g_rules_mu)
std::vector<std::unique_ptr<const std::vector<rolls::Rule>>> g_retired;  // g_rules_mu
std::atomic<const std::vector<rolls::Rule>*> g_published{nullptr};
char g_rolls_why[160] = "feature items.gear.rolls is off (items/PideRarity not hooked)";
struct RollCounters {
    std::atomic<uint64_t> rolled[rolls::kRarities] = {};
    std::atomic<uint64_t> kept{0}, no_slot{0}, faults{0};
} R;

// ItemInfo.perkReferences' offset by this build's reflection (pide_install_rolls sets the real one; tests set
// g_perk_refs themselves). false + why: not found.
bool (*g_resolve_perk_refs)(uint32_t* offset, char* why, size_t cap) = nullptr;

void apply_roll(safetyhook::Context& c) {
    const auto* rules = g_published.load(std::memory_order_acquire);
    const auto* info = reinterpret_cast<const uint8_t*>(c.rdi);
    if (!rules || !info || !g_perk_refs) return;
    uint32_t item;
    std::memcpy(&item, info, 4);  // ItemInfo.itemId (+0; the UsedItem override createPide builds reads the same)
    const rolls::Rule* r = rolls::Table::find(*rules, item);
    if (!r) return;
    const uint32_t word = static_cast<uint32_t>(c.rbx) & 0xFFFFu;  // the asked rarity: {u8 value, u8 has}
    const uint32_t source = rolls::source_of(t_item != 0 && t_item == item, (word >> 8) != 0);
    if (!(r->sources & source)) {
        R.kept.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint32_t allowed = 0;
    for (int k = 0; k < rolls::kRarities; ++k) {
        uint64_t a, b;
        std::memcpy(&a, info + g_perk_refs + 16 * k, 8);
        std::memcpy(&b, info + g_perk_refs + 16 * k + 8, 8);
        if (a | b) allowed |= 1u << k;
    }
    const int rarity = rolls::pick(r->weights, allowed, g_rand());
    if (rarity < 0) {
        R.no_slot.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    c.rbx = (c.rbx & ~uintptr_t{0xFFFF}) | 0x100u | static_cast<uint32_t>(rarity);
    R.rolled[rarity].fetch_add(1, std::memory_order_relaxed);
}

void hk_rarity(safetyhook::Context& c) {
    if (!g_published.load(std::memory_order_relaxed)) return;  // no rules: one load per created item
    __try {
        apply_roll(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        R.faults.fetch_add(1, std::memory_order_relaxed);
    }
}

// g_rules_mu held.
void publish_rules() {
    const auto& merged = g_rules.merged();
    if (merged.empty()) {
        g_published.store(nullptr, std::memory_order_release);
        return;
    }
    auto copy = std::make_unique<const std::vector<rolls::Rule>>(merged);
    g_published.store(copy.get(), std::memory_order_release);
    g_retired.push_back(std::move(copy));
}

// g_rules_mu held. Rolls act: the hook is in and the perk slots' offset is known (resolved at the first set).
bool rolls_ready() {
    if (!g_rolls_hooked) return false;
    if (g_perk_refs) return true;
    uint32_t offset = 0;
    if (!g_resolve_perk_refs) {
        std::snprintf(g_rolls_why, sizeof g_rolls_why, "ItemInfo.perkReferences cannot be looked up here");
        return false;
    }
    if (!g_resolve_perk_refs(&offset, g_rolls_why, sizeof g_rolls_why)) return false;
    g_perk_refs = offset;
    g_rolls_why[0] = 0;
    return true;
}

float roll_chance(SocketKind k) {
    if (k < kKindRings || k > kKindOther) return 0;
    const uint32_t bits = g_roll_bits[k].load(std::memory_order_relaxed);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Points an existing entry at `data` (its old data may be the game's: an ItemInfo default or a shared buffer, so it is
// never written through), or appends {hash, data, 4} while the list has room. false: neither.
bool put4(Override* list, uint64_t& count, uint64_t cap, Override* existing, uint32_t hash, void* data) {
    if (existing) {
        if (existing->size != 4) return false;  // another layout than the 4-byte one this build measured: left alone
        existing->data = data;
        return true;
    }
    if (count >= cap) return false;
    list[count] = Override{hash, 0, data, 4};
    ++count;
    return true;
}

bool layout_ok(int64_t list, int64_t count, int64_t category) {
    // sigdb's d8 captures are unsigned bytes; the instructions sign-extend them as rbp displacements.
    return list >= 0 && list <= 255 && count >= 0 && count <= 255 && static_cast<int8_t>(count) <= 111 &&
        static_cast<int8_t>(count) == static_cast<int8_t>(list) + 8 && category > 0 && category <= 0x1000;
}

void apply(safetyhook::Context& c) {
    const auto rbp = static_cast<uintptr_t>(c.rbp);
    auto*& list = *reinterpret_cast<Override**>(rbp + static_cast<int8_t>(g_list));
    auto& count = *reinterpret_cast<uint64_t*>(rbp + static_cast<int8_t>(g_count));
    const uint64_t cap = *reinterpret_cast<const uint64_t*>(rbp + static_cast<int8_t>(g_count) + 8);
    const auto* info = reinterpret_cast<const uint8_t*>(c.rdi);
    if (!list || count > 64 || cap < count) return;
    Override* perks = nullptr;
    Override* level = nullptr;
    Override* dye = nullptr;
    Override* durability = nullptr;
    Override* state = nullptr;
    bool gem = false, own_item = false;
    for (uint64_t i = 0; i < count; ++i) {
        if (list[i].hash == 0x3a2fac75u && list[i].size == 20) state = &list[i];
        else if (list[i].hash == kPerkContainerNew && list[i].size >= 2) perks = &list[i];
        else if (list[i].hash == kLevel && list[i].size >= 16) level = &list[i];
        else if (list[i].hash == kWeaponGemSlot) gem = true;
        else if (list[i].hash == kDye) dye = &list[i];
        else if (list[i].hash == kDurabilityInit) durability = &list[i];
        else if (list[i].hash == kUsedItem && list[i].size >= 4 && list[i].data)
            own_item = t_item != 0 && *static_cast<const uint32_t*>(list[i].data) == t_item;
    }
    // v7: a gem slot on a non-weapon the game creates (any creation, no give scope): gemSlotOverride, or a roll
    if (!gem && info && g_override && count < cap) {
        const uint8_t category = info[g_category];
        const SocketKind kind = socket_kind(
            category, g_category == kCategoryAtFixedLayout ? info[g_category + kEquipSlotAfterCategory] : 0xFF);
        const bool scoped_any = own_item && (t_flags & KILN_ITEMS_GEM_SLOT) && (t_flags & KILN_ITEMS_GEM_SLOT_ANY);
        if (kind != kKindNone && kind != kKindWeapon) {
            std::atomic<uint64_t>* counter = nullptr;
            if (scoped_any) counter = &C.any;
            else if (info[g_override] != 0) counter = &C.overrides;
            else if (g_roll_any.load(std::memory_order_relaxed)) {
                const float chance = roll_chance(kind);
                if (chance > 0 && g_rand() < chance) counter = &C.rolled;
            }
            if (counter) {
                list[count] = Override{kWeaponGemSlot, 0, g_zero_gem, 12};
                ++count;
                gem = true;
                counter->fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (!own_item) return;
    // createPide supplies this LOCAL ItemState override (not a template's shared default). A new equipped item has
    // no pide when op7 is queued, so give it the same IsEquipped bit vanilla Equip sets on an existing instance.
    if (t_equipped && state && state->data) {
        auto* flags = static_cast<uint8_t*>(state->data) + 0x0e;
        flags[0] |= 8;
    }
    // Both parts of an upgrade must be understood before changing either one.
    if ((t_flags & KILN_ITEMS_UNLOCK) && perks && perks->data && level && level->data && g_level_index && *g_level_index < 2) {
        auto* p = static_cast<uint8_t*>(perks->data);
        const uint32_t available = p[0] > kMaxPerks ? kMaxPerks : p[0];
        const uint32_t want = t_unlock == 255 ? available : (t_unlock < available ? t_unlock : available);
        const uint32_t added = want > p[1] ? want - p[1] : 0;
        if (added) {
            p[1] = static_cast<uint8_t>(want);
            auto* v = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(level->data) + 8);
            v[*g_level_index] += added;
            C.unlocked.fetch_add(added, std::memory_order_relaxed);
        }
    }
    if ((t_flags & KILN_ITEMS_GEM_SLOT) && !gem && info && info[g_category] == kWeapons && count < cap) {
        list[count] = Override{kWeaponGemSlot, 0, g_zero_gem, 12};
        ++count;
        C.gems.fetch_add(1, std::memory_order_relaxed);
    }
    // v7 extras after the gem slot (the list has 8 inline entries; the slot matters most): durability, then the dye
    if (t_flags & KILN_ITEMS_DURABILITY) {
        if (put4(list, count, cap, durability, kDurabilityInit, &t_durability)) C.extras.fetch_add(1, std::memory_order_relaxed);
    }
    if (t_flags & KILN_ITEMS_DYE) {
        if (put4(list, count, cap, dye, kDye, &t_dye)) C.extras.fetch_add(1, std::memory_order_relaxed);
    }
}

void apply_guarded(safetyhook::Context& c) {
    __try {
        apply(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        C.faults.fetch_add(1, std::memory_order_relaxed);
    }
}

void hk_spawn(safetyhook::Context& c) {
    // v7: with feature gems.sockets every creation is looked at (one byte of ItemInfo; gemSlotOverride and rolls)
    if (t_equipped || (t_flags & kPideFlags) || g_override) apply_guarded(c);
}

}  // namespace

bool pide_install(const KilnApi* api, KilnOwner owner) {
    auto sym = [api](const char* n, uintptr_t* out) {
        return api->symbol(KilnStr{n, std::strlen(n)}, out) == KILN_OK && *out;
    };
    auto cst = [api](const char* n, int64_t* out) { return api->constant(KilnStr{n, std::strlen(n)}, out) == KILN_OK; };
    uintptr_t site = 0, index = 0;
    if (!api->feature(KilnStr{"items.pide_options", 18}) || !sym("items/PideSpawn", &site) || !sym("items/PideLevelIndex", &index) ||
        !cst("items/pide.list", &g_list) || !cst("items/pide.count", &g_count) || !cst("items/item.category", &g_category))
        return false;
    // plausibility: rbp-relative slots of the list (pointer, count, capacity follow each other), a byte field of ItemInfo
    if (!layout_ok(g_list, g_count, g_category)) return false;
    g_level_index = reinterpret_cast<const uint32_t*>(index);
    // v7: gemSlotOverride's offset; without it (feature gems.sockets off) the v7 options are refused
    int64_t gem_override = 0;
    const bool sockets = api->feature(KilnStr{"gems.sockets", 12}) && cst("items/item.gem_override", &gem_override) &&
                         gem_override > g_category && gem_override < 0x1000;
    g_ok = h_spawn.install(api, owner, site, &hk_spawn, "items/PideSpawn");
    if (g_ok && sockets) g_override = gem_override;
    return g_ok;
}

bool pide_available() { return g_ok; }
bool pide_sockets_available() { return g_ok && g_override != 0; }

bool pide_request_valid(uint32_t flags, uint8_t unlock) {
    if ((flags & kPideFlags) && (flags & KILN_ITEMS_RECIPE)) return false;
    if ((flags & KILN_ITEMS_GEM_SLOT_ANY) && !(flags & KILN_ITEMS_GEM_SLOT)) return false;
    return (flags & KILN_ITEMS_UNLOCK) ? ((unlock >= 1 && unlock <= 5) || unlock == 255) : unlock == 0;
}

void pide_begin(const PideOptions& o) {
    t_equipped = g_ok && o.equipped;
    t_flags = g_ok ? (o.flags & kPideFlags) : 0;
    t_unlock = o.unlock;
    t_item = o.item;
    t_dye = o.dye;
    t_durability = o.durability;
}

void pide_end() {
    t_equipped = false;
    t_flags = 0;
    t_unlock = 0;
    t_item = 0;
    t_dye = 0;
    t_durability = 1;
}

void pide_commit(void* tx, void(KILN_CALL* commit)(void*), const PideOptions& o) {
    const auto previous_equipped = t_equipped;
    const auto previous_flags = t_flags;
    const auto previous_unlock = t_unlock;
    const auto previous_item = t_item;
    const auto previous_dye = t_dye;
    const auto previous_durability = t_durability;
    pide_begin(o);
#ifdef KILN_ITEMS_HOST_TEST
    try { commit(tx); }
    catch (...) {
        t_equipped = previous_equipped; t_flags = previous_flags; t_unlock = previous_unlock; t_item = previous_item;
        t_dye = previous_dye; t_durability = previous_durability;
        throw;
    }
    t_equipped = previous_equipped; t_flags = previous_flags; t_unlock = previous_unlock; t_item = previous_item;
    t_dye = previous_dye; t_durability = previous_durability;
#else
    __try { commit(tx); }
    __finally {
        t_equipped = previous_equipped; t_flags = previous_flags;
        t_unlock = previous_unlock;
        t_item = previous_item;
        t_dye = previous_dye;
        t_durability = previous_durability;
    }
#endif
}

void pide_commit(void* tx, void(KILN_CALL* commit)(void*), uint32_t flags, uint8_t unlock, uint32_t item) {
    PideOptions o;
    o.flags = flags;
    o.unlock = unlock;
    o.item = item;
    pide_commit(tx, commit, o);
}

void pide_counters(uint64_t* unlocked, uint64_t* gems, uint64_t* faults) {
    *unlocked = C.unlocked.load();
    *gems = C.gems.load();
    *faults = C.faults.load();
}

void pide_socket_counters(uint64_t* by_give, uint64_t* by_override, uint64_t* by_roll, uint64_t* extras) {
    *by_give = C.any.load();
    *by_override = C.overrides.load();
    *by_roll = C.rolled.load();
    *extras = C.extras.load();
}

SocketKind socket_kind(uint8_t category, uint8_t equip_slot) {
    switch (category) {
        case kWeapons: return kKindWeapon;
        case 0:   // Customization (body parts, hair)
        case 15:  // WeaponGem: a gem never holds a gem
        case 16:  // ColorPalette (dyes)
            return kKindNone;
        case kEquipment:
            if (equip_slot == 37) return kKindRings;                       // Armour_Ring
            if (equip_slot >= 13 && equip_slot <= 17) return kKindArmour;  // Armour_Head .. Armour_Feet
            if (equip_slot == 18) return kKindShields;                     // Shield
            if (equip_slot == 0xFF) return kKindNone;                      // layout unknown: no roll by kind
            return kKindOther;
        default: return kKindOther;
    }
}

KilnResult pide_socket_roll(KilnOwner owner, const KilnItemsSocketRoll* roll) {
    if (roll) {
        // availability first: a caller can ask "is it on?" with an invalid roll and change nothing (Kiln.Items does)
        if (!pide_sockets_available()) return KILN_ERR_UNAVAILABLE;
        if (roll->size < sizeof(KilnItemsSocketRoll) || roll->reserved != 0) return KILN_ERR_INVALID_ARGUMENT;
        for (float f : {roll->rings, roll->armour, roll->shields, roll->other})
            if (!(f >= 0.0f && f <= 1.0f)) return KILN_ERR_INVALID_ARGUMENT;  // NaN fails too
    }
    std::lock_guard lock(g_roll_mu);
    if (!roll || (roll->rings == 0 && roll->armour == 0 && roll->shields == 0 && roll->other == 0)) g_rolls.erase(owner);
    else g_rolls[owner] = *roll;
    float best[4] = {};
    for (const auto& [o, r] : g_rolls) {
        const float v[4] = {r.rings, r.armour, r.shields, r.other};
        for (int k = 0; k < 4; ++k) best[k] = v[k] > best[k] ? v[k] : best[k];
    }
    bool any = false;
    for (int k = 0; k < 4; ++k) {
        uint32_t bits;
        std::memcpy(&bits, &best[k], 4);
        g_roll_bits[k].store(bits, std::memory_order_relaxed);
        any |= best[k] > 0;
    }
    g_roll_any.store(any, std::memory_order_relaxed);
    return KILN_OK;
}

// ---- v8 (P5): rarity rolls at creation ----

#ifndef KILN_ITEMS_HOST_TEST
namespace {
const KilnApi* g_types_api = nullptr;

// ItemInfo.perkReferences by kiln.types: 5 entries of 16 bytes, the slots createPide tests; and ItemInfo.itemId at +0.
bool resolve_perk_refs(uint32_t* offset, char* why, size_t cap) {
    const auto* t = g_types_api ? static_cast<const KilnTypesApi*>(g_types_api->service(KilnStr{KILN_TYPES_SERVICE, std::strlen(KILN_TYPES_SERVICE)}, 1))
                                : nullptr;
    if (!t || t->header.size < sizeof(KilnTypesApi)) return std::snprintf(why, cap, "kiln.types is not loaded"), false;
    const KilnType* info = t->type_by_name(KilnStr{"keen::ItemInfo", 14});
    KilnTypeInfo ti{};
    ti.size = sizeof ti;
    if (!info || t->type_info(info, &ti) != KILN_OK) return std::snprintf(why, cap, "this build has no keen::ItemInfo"), false;
    bool id_ok = false;
    uint32_t refs = 0, refs_size = 0;
    for (uint32_t i = 0; i < ti.field_count; ++i) {
        KilnTypeField f{};
        if (t->type_field(info, i, &f) != KILN_OK) continue;
        KilnTypeInfo fi{};
        fi.size = sizeof fi;
        if (t->type_info(f.type, &fi) != KILN_OK) continue;
        const std::string_view name(f.name.ptr, f.name.len);
        if (name == "itemId") id_ok = f.offset == 0 && fi.type_size == 4;
        else if (name == "perkReferences") refs = static_cast<uint32_t>(f.offset), refs_size = fi.type_size;
    }
    if (!id_ok) return std::snprintf(why, cap, "this build's ItemInfo.itemId is not 4 bytes at +0"), false;
    if (refs == 0 || refs_size != 5 * 16 || refs + refs_size > 0x2000)
        return std::snprintf(why, cap, "this build's ItemInfo.perkReferences is not 5 x 16 bytes"), false;
    *offset = refs;
    return true;
}
}  // namespace
#endif

bool pide_install_rolls(const KilnApi* api, KilnOwner owner) {
    uintptr_t site = 0;
    if (!api->feature(KilnStr{"items.gear.rolls", 16}) ||
        api->symbol(KilnStr{"items/PideRarity", 16}, &site) != KILN_OK || !site)
        return false;
#ifndef KILN_ITEMS_HOST_TEST
    g_types_api = api;
    g_resolve_perk_refs = &resolve_perk_refs;
#endif
    std::lock_guard lock(g_rules_mu);
    g_rolls_hooked = h_rarity.install(api, owner, site, &hk_rarity, "items/PideRarity");
    if (g_rolls_hooked) std::snprintf(g_rolls_why, sizeof g_rolls_why, "rule sets resolve ItemInfo.perkReferences first");
    else std::snprintf(g_rolls_why, sizeof g_rolls_why, "items/PideRarity could not be hooked: %s", h_rarity.error() ? h_rarity.error() : "?");
    return g_rolls_hooked;
}

KilnResult pide_rarity_rolls(KilnOwner owner, const KilnItemsRarityRoll* rolls_in, uint32_t count, uint32_t roll_size) {
    if (!rolls_in || count == 0) {  // ending a set always works, also when rolls are off
        std::lock_guard lock(g_rules_mu);
        g_rules.set(owner, {});
        publish_rules();
        return KILN_OK;
    }
    {
        std::lock_guard lock(g_rules_mu);
        if (!rolls_ready()) return KILN_ERR_UNAVAILABLE;
    }
    if (roll_size < sizeof(KilnItemsRarityRoll) || count > rolls::kMaxRules) return KILN_ERR_INVALID_ARGUMENT;
    std::vector<rolls::Rule> set;
    set.reserve(count);
    std::unordered_set<uint32_t> seen;
    for (uint32_t i = 0; i < count; ++i) {
        KilnItemsRarityRoll e;
        std::memcpy(&e, reinterpret_cast<const uint8_t*>(rolls_in) + static_cast<size_t>(i) * roll_size, sizeof e);
        if (!rolls::valid(e) || !seen.insert(e.item).second) return KILN_ERR_INVALID_ARGUMENT;
        rolls::Rule r;
        r.item = e.item;
        r.sources = e.sources;
        std::memcpy(r.weights, e.weights, sizeof r.weights);
        set.push_back(r);
    }
    std::lock_guard lock(g_rules_mu);
    g_rules.set(owner, std::move(set));
    publish_rules();
    return KILN_OK;
}

KilnResult pide_roll_status(KilnItemsRollStatus* out) {
    if (!out || out->size < sizeof(KilnItemsRollStatus)) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    std::lock_guard lock(g_rules_mu);
    out->available = g_rolls_hooked && (g_perk_refs || g_resolve_perk_refs) ? 1 : 0;
    out->items = static_cast<uint32_t>(g_rules.merged().size());
    out->owners = g_rules.owners();
    out->conflicts = g_rules.conflicts();
    for (int r = 0; r < rolls::kRarities; ++r) out->rolled[r] = R.rolled[r].load();
    out->kept = R.kept.load();
    out->no_slot = R.no_slot.load();
    out->faults = R.faults.load();
    if (!g_perk_refs || !g_rolls_hooked) std::snprintf(out->detail, sizeof out->detail, "%s", g_rolls_why);
    return KILN_OK;
}

}  // namespace kitems
