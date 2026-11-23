// SPDX-License-Identifier: MIT
// kiln.items.inventory_kinds (kiln_items_inventory_kinds.h, GitHub issue #138), the pure part (inventory_kinds_logic.h)
// on the host, no game: the layout the managed side and the ABI ledger pin, the kind order (only proven facts, UNKNOWN
// otherwise), and find's bounded caller buffer (count, truncation, count-only, bad arguments).
//   g++ -std=c++20 -I native/core/abi native/services/items/tests/inventory_kinds_test.cpp && ./a.out
#include <cstdio>
#include <cstddef>
#include <vector>

#include "../inventory_kinds_logic.h"

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_failed;                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                     \
    } while (0)

using namespace kitems::kinds;

int main() {
    // the approved layout (dev.31 DECISIONS: x64 16 B, header at 0, find @8; rows 8 B {u32 entity, u32 kind})
    CHECK(sizeof(KilnItemsInventoryKindsApi) == 16);
    CHECK(offsetof(KilnItemsInventoryKindsApi, header) == 0);
    CHECK(offsetof(KilnItemsInventoryKindsApi, find) == 8);
    CHECK(sizeof(KilnInventoryKindRow) == 8 && offsetof(KilnInventoryKindRow, entity) == 0 && offsetof(KilnInventoryKindRow, kind) == 4);
    // the stable codes
    CHECK(KILN_INVENTORY_KIND_UNKNOWN == 0 && KILN_INVENTORY_KIND_PLAYER == 1 && KILN_INVENTORY_KIND_NPC == 2 &&
          KILN_INVENTORY_KIND_PLAYER_STORAGE == 3 && KILN_INVENTORY_KIND_PRODUCTION_STATION == 4 && KILN_INVENTORY_KIND_BASE_STORAGE == 5);

    // classification: only proven facts, in the header's order
    Facts none;
    CHECK(classify(none) == KILN_INVENTORY_KIND_UNKNOWN);  // an ordinary chest: no proof of what it is
    Facts setup0;
    setup0.setup = true;
    CHECK(classify(setup0) == KILN_INVENTORY_KIND_UNKNOWN);
    Facts player = setup0;
    player.setup_flags = 1;
    CHECK(classify(player) == KILN_INVENTORY_KIND_PLAYER);
    Facts npc = setup0;
    npc.setup_flags = 2;
    CHECK(classify(npc) == KILN_INVENTORY_KIND_NPC);
    Facts both = setup0;
    both.setup_flags = 3;
    CHECK(classify(both) == KILN_INVENTORY_KIND_PLAYER);
    Facts preview = setup0;
    preview.setup_flags = 4 | 8;  // ShowContentPreview, IndestructibleWhileNotEmpty: no kind
    CHECK(classify(preview) == KILN_INVENTORY_KIND_UNKNOWN);
    Facts unread;
    unread.setup_flags = 1;  // flags not read (setup false): not a player
    CHECK(classify(unread) == KILN_INVENTORY_KIND_UNKNOWN);
    Facts holder = player;
    holder.holder = true;
    CHECK(classify(holder) == KILN_INVENTORY_KIND_PLAYER_STORAGE);
    Facts station = setup0;
    station.factory = true;
    station.base_stock = true;
    CHECK(classify(station) == KILN_INVENTORY_KIND_PRODUCTION_STATION);
    Facts magic = setup0;
    magic.base_stock = true;
    CHECK(classify(magic) == KILN_INVENTORY_KIND_BASE_STORAGE);
    Facts npc_station = npc;
    npc_station.factory = true;
    CHECK(classify(npc_station) == KILN_INVENTORY_KIND_NPC);

    // arguments
    KilnInventoryKindRow rows[4] = {};
    CHECK(args_ok(0, nullptr, 0));
    CHECK(args_ok(0x1F, rows, 4));
    CHECK(!args_ok(0x20, rows, 4));
    CHECK(!args_ok(1, nullptr, 1));

    // the bounded buffer
    const KilnEntity ids[3] = {11, 12, 13};
    int asked = 0;
    auto kind = [&](KilnEntity e) {
        ++asked;
        return e == 12 ? static_cast<uint32_t>(KILN_INVENTORY_KIND_PRODUCTION_STATION) : static_cast<uint32_t>(KILN_INVENTORY_KIND_UNKNOWN);
    };
    uint32_t count = 99;
    CHECK(fill(ids, 3, kind, rows, 4, &count) == KILN_OK && count == 3 && asked == 3);
    CHECK(rows[0].entity == 11 && rows[1].entity == 12 && rows[1].kind == KILN_INVENTORY_KIND_PRODUCTION_STATION && rows[2].kind == 0);
    rows[0] = rows[1] = rows[2] = rows[3] = KilnInventoryKindRow{7, 7};
    asked = 0;
    CHECK(fill(ids, 3, kind, rows, 2, &count) == KILN_ERR_BUFFER_TOO_SMALL && count == 3 && asked == 2);  // truncated
    CHECK(rows[0].entity == 11 && rows[1].entity == 12 && rows[2].entity == 7);                           // nothing past capacity
    asked = 0;
    CHECK(fill(ids, 3, kind, nullptr, 0, &count) == KILN_ERR_BUFFER_TOO_SMALL && count == 3 && asked == 0);  // count only
    CHECK(fill(ids, 0, kind, nullptr, 0, &count) == KILN_OK && count == 0);
    CHECK(fill(ids, 3, kind, rows, 3, nullptr) == KILN_OK);  // out_count may be NULL

    std::printf("inventory_kinds_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
