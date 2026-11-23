// SPDX-License-Identifier: MIT
// kiln.items: gear options at creation (pide.cpp): KILN_ITEMS_UNLOCK and KILN_ITEMS_GEM_SLOT; v7 (the gem wave):
// KILN_ITEMS_GEM_SLOT_ANY, KILN_ITEMS_DYE, KILN_ITEMS_DURABILITY, gemSlotOverride on any gear, socket rolls; v8 (P5):
// rarity rolls at creation.
#pragma once
#include <cstdint>

#include "kiln_api.h"
#include "services/kiln_items.h"

namespace kitems {
// The request flags the createPide hook acts on.
constexpr uint32_t kPideFlags = KILN_ITEMS_UNLOCK | KILN_ITEMS_GEM_SLOT | KILN_ITEMS_GEM_SLOT_ANY | KILN_ITEMS_DYE |
                                KILN_ITEMS_DURABILITY;

// One give's options for the item it creates (scoped to that item id and thread).
struct PideOptions {
    uint32_t flags = 0;  // KILN_ITEMS_* (only kPideFlags are used)
    uint8_t unlock = 0;
    uint32_t item = 0;
    uint32_t dye = 0;          // KILN_ITEMS_DYE
    float durability = 1.0f;   // KILN_ITEMS_DURABILITY: 0..1
    bool equipped = false;     // GiveEquip only: initial ItemState.IsEquipped, before its queued spawn
    uint64_t equipped_slot = 0; // exact final cell: re-entrant creations of the same item must not inherit it
};

bool pide_install(const KilnApi* api, KilnOwner owner);  // feature items.pide_options; false: the options are refused
bool pide_available();
bool pide_sockets_available();  // v7: feature gems.sockets (the gemSlotOverride offset) on top of pide_available()
bool pide_request_valid(uint32_t flags, uint8_t unlock);
// Scoped to this give's item and thread. Cleanup also runs when the engine commit raises a structured exception.
void pide_commit(void* tx, void(KILN_CALL* commit)(void*), const PideOptions& options);
void pide_commit(void* tx, void(KILN_CALL* commit)(void*), uint32_t flags, uint8_t unlock, uint32_t item);
void pide_begin(const PideOptions& options);
void pide_end();
void pide_counters(uint64_t* unlocked, uint64_t* gems, uint64_t* faults);
// v7: sockets added on non-weapons (by a give's GEM_SLOT_ANY, by gemSlotOverride, by a roll) and the extras applied
void pide_socket_counters(uint64_t* by_give, uint64_t* by_override, uint64_t* by_roll, uint64_t* extras);
// v7: socket_roll of the table. NULL ends the owner's rolls (also what cancel_owner does).
KilnResult pide_socket_roll(KilnOwner owner, const KilnItemsSocketRoll* roll);

// v8 (P5): rarity rolls at creation (kiln_items.h "rarity rolls"). install: feature items.gear.rolls and the mid hook at
// items/PideRarity; false: rolls stay off (rarity_rolls answers KILN_ERR_UNAVAILABLE). rarity_rolls(owner, NULL, 0, 0)
// ends the owner's rules (cancel_owner does it).
bool pide_install_rolls(const KilnApi* api, KilnOwner owner);
KilnResult pide_rarity_rolls(KilnOwner owner, const KilnItemsRarityRoll* rolls, uint32_t count, uint32_t roll_size);
KilnResult pide_roll_status(KilnItemsRollStatus* out);

// The kind of gear an ItemInfo is, for rolls and GEM_SLOT_ANY: by category and equipment slot type (0xFF: unknown).
enum SocketKind : int { kKindNone = -1, kKindRings = 0, kKindArmour = 1, kKindShields = 2, kKindOther = 3, kKindWeapon = 4 };
SocketKind socket_kind(uint8_t category, uint8_t equip_slot);
}  // namespace kitems
