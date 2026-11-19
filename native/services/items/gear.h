// SPDX-License-Identifier: MIT
// kiln.items v8 (since 0.2.0-dev.29.19, the equipment wave): an item entity's rarity, enhancements and level, written in
// place inside the serial program kiln_items_gear (kiln.world). ABI: kiln_items.h "gear"; rules without game memory in
// gear_logic.h (host test tests/gear_test.cpp); evidence docs/re/items.md section 19.
#pragma once
#include <cstdint>

#include "kiln_api.h"
#include "services/kiln_items.h"
#include "services/kiln_world.h"

namespace kitems {

// resolve(): feature items.gear (symbols and constants), after the instance symbols.
void gear_resolve();
// on_ready: registers kiln_items_gear through kiln.world (no kiln.world: gear writes stay off).
void gear_register(const KilnWorldApi* w);
// Tick thread, Pre: the program's state, timeouts, results.
void gear_pre(uint64_t step);
// The server world closed: waiting writes end DROPPED.
void gear_world_closed();
// cancel(ticket) / cancel_owner(owner): drop callbacks. gear_cancel is true when the ticket was a gear write's.
bool gear_cancel(uint64_t ticket);
void gear_cancel_owner(KilnOwner owner);

KilnResult gear_write(KilnOwner owner, const KilnItemsGearRequest* request, KilnItemsGearDoneFn done, void* user,
                      uint64_t* out_ticket);
KilnResult gear_read(KilnEntity entity, uint32_t stamp, KilnItemsGearState* out);
KilnResult gear_status(KilnItemsGearStatus* out);
// v8 (package P3): a gear write paid for, in one step of kiln_items_gear (kiln_items.h "upgrades").
KilnResult gear_upgrade(KilnOwner owner, const KilnItemsUpgradeRequest* request, KilnItemsUpgradeDoneFn done, void* user,
                        uint64_t* out_ticket);

}  // namespace kitems
