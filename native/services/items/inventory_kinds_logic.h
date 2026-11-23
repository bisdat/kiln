// SPDX-License-Identifier: MIT
// kiln.items.inventory_kinds (kiln_items_inventory_kinds.h, GitHub issue #138): the pure part, host-tested
// (tests/inventory_kinds_test.cpp): which kind the facts Kiln read about one inventory owner prove, and the bounded
// caller-buffer answer. items.cpp reads the facts (kiln.items' find_inventories, InventorySetup's flags, kiln.items.storage's
// holders, kiln.stations' FactoryStation and the game's base lists) on the tick thread.
#pragma once
#include <cstdint>

#include "../../core/abi/services/kiln_items_inventory_kinds.h"

namespace kitems::kinds {

// What was read about one owner find_inventories matched.
struct Facts {
    bool holder = false;      // a kiln.items.storage holder
    bool setup = false;       // its InventorySetup flags were read
    uint8_t setup_flags = 0;  // InventorySetup.inventoryFlags (bit 0 IsPlayer, bit 1 IsNpc)
    bool factory = false;     // carries FactoryStation
    bool base_stock = false;  // in a base's crafting-stock list this frame
};

// The first proven kind, in the header's order; UNKNOWN when nothing is proven.
constexpr uint32_t classify(const Facts& f) {
    if (f.holder) return KILN_INVENTORY_KIND_PLAYER_STORAGE;
    if (f.setup && (f.setup_flags & 1u)) return KILN_INVENTORY_KIND_PLAYER;
    if (f.setup && (f.setup_flags & 2u)) return KILN_INVENTORY_KIND_NPC;
    if (f.factory) return KILN_INVENTORY_KIND_PRODUCTION_STATION;
    if (f.base_stock) return KILN_INVENTORY_KIND_BASE_STORAGE;
    return KILN_INVENTORY_KIND_UNKNOWN;
}

// find's argument check: out NULL with a capacity, or a flag bit outside find_inventories' 0x1F.
constexpr bool args_ok(uint32_t flags, const KilnInventoryKindRow* out, uint32_t capacity) {
    return !(capacity && !out) && !(flags & ~0x1Fu);
}

// The answer for the n owners find_inventories matched: rows for the first capacity of them (kind_of(entity) each),
// *out_count = n; KILN_ERR_BUFFER_TOO_SMALL past capacity. kind_of is called only for rows written.
template <class KindOf>
KilnResult fill(const KilnEntity* ids, uint32_t n, KindOf kind_of, KilnInventoryKindRow* out, uint32_t capacity, uint32_t* out_count) {
    const uint32_t w = n < capacity ? n : capacity;
    for (uint32_t i = 0; i < w; ++i) out[i] = KilnInventoryKindRow{ids[i], kind_of(ids[i])};
    if (out_count) *out_count = n;
    return n > capacity ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}

}  // namespace kitems::kinds
