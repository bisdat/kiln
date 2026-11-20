// SPDX-License-Identifier: MIT
// kiln.items v8 (since 0.2.0-dev.29.19, the equipment wave, package P7): an item entity's power and armour scales, read
// by a detour of the game's item power (items/ItemPower) and two mid hooks around one armour piece's part of apply_armor
// (items/ArmorSlotBase, items/ArmorSlotEnd). ABI: kiln_items.h "power and armour scales"; the table without game memory
// in scale_logic.h (host test tests/gear_test.cpp); evidence docs/re/items.md section 20.
#pragma once
#include <cstdint>

#include "kiln_api.h"
#include "services/kiln_items.h"

namespace kitems {

// resolve(): features items.gear.power and items.gear.armor (symbols, captured offsets, this build's reflection). Nothing
// is hooked here: the first scale other than 1 of a kind installs its hooks.
void scale_resolve(const KilnApi* api, KilnOwner owner);
// A server world closed or started: every scale is forgotten (entity ids start again).
void scale_world_reset();
// Tick thread, Pre: the stamp table forgot its entries (its epoch moved: the id counter went down): so does this table.
void scale_pre();

KilnResult scale_set(const KilnItemsGearScale* s);
KilnResult scale_read(KilnEntity entity, uint32_t stamp, KilnItemsGearScale* out);
KilnResult scale_status(KilnItemsScaleStatus* out);

}  // namespace kitems
