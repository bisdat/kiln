/* SPDX-License-Identifier: MIT
 * kiln.items.inventory_kinds v1 (since 0.2.0-dev.31, GitHub issue #138): the inventory owners kiln.items'
 * find_inventories finds, each with the kind Kiln can prove for it. Served by kiln-items.dll. A table of its own: kiln.items
 * does not grow while dev.30 is open. Capability: items.inventory_kinds. Managed face: Kiln.Items
 * (Items.FindInventoryOwners, InventoryOwnerInfo, InventoryOwnerKind; Bases.Chests). docs/kiln/services/items.md.
 *
 *   find  the owners find_inventories(flags) matches (KILN_ITEMS_FIND_* of kiln_items.h, with
 *         KILN_ITEMS_FIND_PLAYER_STORAGE of kiln_items_storage.h; 0 = all but player storage holders), in no particular
 *         order, as rows {entity, kind}. *out_count = how many match. More than capacity: the first capacity rows are
 *         written and KILN_ERR_BUFFER_TOO_SMALL is returned (ask again with capacity >= *out_count). capacity 0 with out
 *         NULL asks for the count only (KILN_ERR_BUFFER_TOO_SMALL when any match, KILN_OK when none). out_count may be
 *         NULL. KILN_ERR_INVALID_ARGUMENT: out NULL with a capacity, or a flag bit outside 0x1F (nothing written, count
 *         0). The guards of find_inventories: KILN_ERR_UNAVAILABLE without its reads or before the server world's first
 *         step, KILN_ERR_WRONG_THREAD off the tick thread (Pre / Post), KILN_ERR_INTERNAL when the world's entities
 *         faulted. One pass over the world's entities and one read of the game's bases.
 *
 * Kinds: append-only stable codes. Only what a component or Kiln's own registry proves; never a template-name guess. An
 * owner Kiln cannot classify is KILN_INVENTORY_KIND_UNKNOWN: ordinary storage chests among them (no component tells a
 * chest from any other plain container), and a base's storage when kiln.stations cannot read the game's bases. Checked in
 * this order, the first that holds:
 *   PLAYER_STORAGE      a player storage holder of kiln.items.storage (Kiln's own registry)
 *   PLAYER              InventorySetup.inventoryFlags IsPlayer (bit 0)
 *   NPC                 InventorySetup.inventoryFlags IsNpc (bit 1)
 *   PRODUCTION_STATION  carries keen::ecs::FactoryStation (what kiln.stations reads as a production station)
 *   BASE_STORAGE        in a base's crafting-stock list as the game's PlayerBasesSystemServer has it this frame (the Magic
 *                       storage chests and the player-planted refilling fruit trees and palms, docs/kiln/services/stations.md)
 * Readers treat a code they do not know as UNKNOWN. */
#ifndef KILN_ITEMS_INVENTORY_KINDS_H
#define KILN_ITEMS_INVENTORY_KINDS_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_ITEMS_INVENTORY_KINDS_SERVICE "kiln.items.inventory_kinds"
#define KILN_ITEMS_INVENTORY_KINDS_VERSION 1u

/* KilnInventoryKindRow.kind */
enum {
    KILN_INVENTORY_KIND_UNKNOWN = 0u,
    KILN_INVENTORY_KIND_PLAYER = 1u,
    KILN_INVENTORY_KIND_NPC = 2u,
    KILN_INVENTORY_KIND_PLAYER_STORAGE = 3u,
    KILN_INVENTORY_KIND_PRODUCTION_STATION = 4u,
    KILN_INVENTORY_KIND_BASE_STORAGE = 5u,
};

typedef struct KilnInventoryKindRow {
    KilnEntity entity; /* 0 */
    uint32_t kind;     /* 4: KILN_INVENTORY_KIND_* */
} KilnInventoryKindRow;

typedef struct KilnItemsInventoryKindsApi {
    KilnServiceHeader header; /* KILN_ITEMS_INVENTORY_KINDS_VERSION; append-only */
    KilnResult(KILN_CALL* find)(uint32_t flags, KilnInventoryKindRow* out, uint32_t capacity, uint32_t* out_count);
} KilnItemsInventoryKindsApi;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
static_assert(sizeof(KilnInventoryKindRow) == 8, "KilnInventoryKindRow is 8 bytes");
static_assert(offsetof(KilnInventoryKindRow, kind) == 4, "kind follows the entity");
static_assert(offsetof(KilnItemsInventoryKindsApi, find) == 8, "find follows the header");
static_assert(sizeof(KilnItemsInventoryKindsApi) == 16, "KilnItemsInventoryKindsApi is 16 bytes on x64");
#endif

#endif /* KILN_ITEMS_INVENTORY_KINDS_H */
