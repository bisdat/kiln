/* SPDX-License-Identifier: MIT
 * kiln.ui.snapshot v1 (in-core service, client only, since 0.2.0-dev.31): live game values for UI blocks (dossier chapter
 * 09 §4.4, §5.1): the local player's vitals, position and own inventory as this game has them, and the hour of the world
 * when this game hosts it, as ONE immutable publication. docs/kiln/modding/ui-blocks.md "Live game values" is the
 * author's page; managed mods read it through Kiln.Blocks' BlockUi.ReadWorld / BlockUi.DemandWorld.
 *
 * Nothing here reads the world when it is called: every slot is any-thread and only hands out what the collector
 * published. The collector reads only what someone demanded (demand_add), on the right world's own thread:
 *   - LOCAL_VITALS, LOCAL_INVENTORY: kiln.ui.values' pre-provider stage of the game's player-UI update (chapter 05a): the
 *     ORIGINAL health/stamina/mana pairs and the local player's own inventory models, copied after the game's update and
 *     before any mod's value provider changed them (a block shows 413 health while a cosmetic HUD provider shows 37).
 *     Nonempty own slots only (never an open container or a detail copy), sorted by (inventory_page, slot_index). More
 *     than 512 slots: inventory is unavailable with KUW_BUDGET, never a partial list. Needs the ui.values.hud feature
 *     (its update hook); without it both are unavailable with KUW_LAYOUT_OFF.
 *   - LOCAL_POSITION: on the client world's Post (after its jobs), the core symbol GetEntityTransform with the CLIENT
 *     world and the local player; the 56-byte CurrentTransform (keen::WorldTransform) is copied and checked (orientation
 *     and scale finite). x/y/z_fixed are its signed 32.32 fixed-point position: metres = value / 4294967296. Never a
 *     server-bound read: a joined client reads its own client world only.
 *   - HOST_TIME: only when this process hosts the world (KILN_SESSION_HOST): kiln.time's get on the hosted server world's
 *     Post, with the server world's own session, step and capture time (never called atomic with the local part). On a
 *     joined world (KILN_SESSION_REMOTE, a Kiln or a vanilla server alike) HOST_TIME is accepted by demand_add but
 *     yields KUW_DEFERRED_SOURCE: v1 has no network subscription for it. The OS clock is never substituted.
 * Collection runs at most every 100 ms, on the client world's Post. The local part is one sample: position (that Post)
 * is combined with the stage's vitals/inventory only when both carry the same client-world session and step; a stage
 * of another step makes those fields unavailable with KUW_STALE_SOURCE (KUW_NO_SNAPSHOT before the first stage), never
 * a mix of two steps. Without a position demand the stage's own step stamps the local part. Each publication gets the
 * next publication id (monotonic for the process). A world or session change (menu, loading screen, rejoin) clears
 * the publication. Fields older than 1000 ms are refused: a whole publication whose local capture is older gives
 * KUW_EXPIRED at acquire; host time older than that is taken out of the pinned view (bit cleared, reason KUW_EXPIRED).
 * Capture times are GetTickCount64 milliseconds; the local stamp is the core's client-world session and step (kiln.client),
 * not a UI frame counter.
 *
 * Demand (demand_add / demand_remove). A handle says "collect these groups" (KilnUiWorldMask bits; 0 or an unknown bit is
 * KILN_ERR_INVALID_ARGUMENT). The collector reads the union of every live handle's mask; no handle, no read. At most 64
 * handles per owner and 1024 in all: over that, KILN_OK with ok = 0 and KUW_BUDGET. A handle goes with demand_remove or
 * when its owner is revoked. HOST_TIME on a joined world: ok = 1 and reason KUW_DEFERRED_SOURCE (accepted, nothing will
 * come).
 *
 * Reading (acquire / copy / release). acquire returns a token that pins ONE whole publication: header and inventory are
 * always of that publication, never a header of one and records of another. At most 2 live tokens per owner and 128 in
 * all; a token lives 2 s (TTL) or until release. No world, no publication yet, a publication older than 1000 ms, or the
 * token budget: KILN_OK with ok = 0, token 0 and the reason (KUW_NO_WORLD, KUW_NO_SNAPSHOT, KUW_EXPIRED, KUW_BUDGET).
 * copy writes the header and inventory_count records into the caller's buffers in one go. When capacity is below the
 * pinned publication's record count it writes NOTHING (neither the header nor a record), sets *required (when given)
 * and returns KILN_ERR_BUFFER_TOO_SMALL; a retry with the same token gets the same publication, also when the collector
 * published another one in between. A world or session change revokes every token at once, even within its TTL: copy
 * answers KILN_ERR_REVOKED and an old token never shows a new character's values. A token whose 2 s ran out, a released
 * or never issued token: KILN_ERR_NOT_FOUND (an Expired refusal: acquire again). release frees the pin
 * (KILN_ERR_NOT_FOUND when there is nothing to free).
 *
 * Records. Every record with a size field carries the caller's sizeof: below v1 is KILN_ERR_INVALID_ARGUMENT; Kiln fills
 * the v1 fields, leaves bytes past v1 untouched and keeps size. KilnUiInventoryRow has no size field: it is 24 bytes for
 * the life of this table (a later record shape comes with a new slot). Read a group's fields only when its bit is in
 * available_mask (the others are zero); unavailable_reason[i] says why bit (1 << i) is missing: index 0 LOCAL_VITALS,
 * 1 LOCAL_POSITION, 2 LOCAL_INVENTORY, 3 HOST_TIME. A group nobody demanded says KUW_NO_SNAPSHOT. requested_mask is the
 * union of demands the publication was collected for.
 *
 * Errors. KILN_ERR_INVALID_ARGUMENT: a null pointer, a size below v1, mask 0 or an unknown bit, token or handle 0,
 * capacity above 0 with rows null. KILN_ERR_REVOKED: the owner was revoked (demand_add, acquire), or the token's world
 * changed (copy). KILN_ERR_UNAVAILABLE: not a game client. KILN_ERR_NOT_FOUND: see above.
 *
 * Capability rows: the service row kiln.ui.snapshot; ui.blocks.world (the table is here), world.vitals and world.inventory
 * (the table and ui.values.hud), world.position (the table and GetEntityTransform; this service reports it unavailable
 * when the symbol does not resolve or the read faulted), world.host_time (the table and kiln.time). Kiln.Blocks reports
 * the composed rows; this service lowers world.vitals and world.inventory when the stage refuses it.
 *
 * The table is append-only: check header.size before calling a member added later than v1. */
#ifndef KILN_UI_SNAPSHOT_H
#define KILN_UI_SNAPSHOT_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_UI_SNAPSHOT_SERVICE "kiln.ui.snapshot"
#define KILN_UI_SNAPSHOT_VERSION 1u

/* Demand and availability bits. */
enum KilnUiWorldMask { KUW_LOCAL_VITALS=1, KUW_LOCAL_POSITION=2,
    KUW_LOCAL_INVENTORY=4, KUW_HOST_TIME=8 };
/* Why a group, an acquire or a demand was refused (acquire.reason, demand.reason, unavailable_reason[]). */
enum KilnUiSnapshotReason { KUW_NONE=0, KUW_NO_WORLD=1, KUW_NO_SNAPSHOT=2,
    KUW_BUDGET=3, KUW_EXPIRED=4, KUW_BUFFER_SMALL=5, KUW_DEFERRED_SOURCE=6,
    KUW_MISSING_COMPONENT=7, KUW_STALE_SOURCE=8, KUW_LAYOUT_OFF=9 };
/* What acquire answers (caller sets size). ok = 1: the token pins publication, whose local part was captured at
 * capture_ms. */
typedef struct KilnUiSnapshotAcquire {
    uint32_t size, ok, reason, reserved; KilnHandle token;
    uint64_t publication, capture_ms;
} KilnUiSnapshotAcquire;
/* One publication's header (caller sets size). 4 bytes of padding follow mana_max (offset 92): x_fixed is at 96. */
typedef struct KilnUiWorldValues {
    uint32_t size, available_mask, requested_mask, inventory_count;
    uint32_t unavailable_reason[4];
    uint64_t publication, local_session, local_step, local_capture_ms;
    uint32_t local_player, health, health_max, stamina, stamina_max, mana, mana_max;
    int64_t x_fixed, y_fixed, z_fixed;
    uint64_t server_session, server_step, server_capture_ms;
    double server_hours;
} KilnUiWorldValues;
/* One nonempty slot of the local player's own inventory. holder: the inventory model's owner (the local player). */
typedef struct KilnUiInventoryRow {
    uint32_t holder, inventory_page, slot_index, item, count, pide;
} KilnUiInventoryRow;
/* What demand_add answers (caller sets size). */
typedef struct KilnUiDemandResult {
    uint32_t size, ok, reason, reserved; KilnHandle handle;
} KilnUiDemandResult;
typedef struct KilnUiSnapshotApi {
    KilnServiceHeader header; /* new kiln.ui.snapshot v1; append-only */
    KilnResult (KILN_CALL *demand_add)(KilnOwner, uint32_t mask, KilnUiDemandResult*);
    KilnResult (KILN_CALL *demand_remove)(KilnHandle);
    KilnResult (KILN_CALL *acquire)(KilnOwner, KilnUiSnapshotAcquire*);
    KilnResult (KILN_CALL *copy)(KilnHandle token, KilnUiWorldValues*,
        KilnUiInventoryRow*, uint32_t capacity, uint32_t* required);
    KilnResult (KILN_CALL *release)(KilnHandle token);
} KilnUiSnapshotApi;

/* ---- layout (x64; every reader checks header.size and record sizes) ---- */
#ifdef __cplusplus
#define KUW_ASSERT static_assert
#else
#define KUW_ASSERT _Static_assert
#endif
KUW_ASSERT(sizeof(KilnUiSnapshotAcquire) == 40 && offsetof(KilnUiSnapshotAcquire, token) == 16 &&
               offsetof(KilnUiSnapshotAcquire, publication) == 24 && offsetof(KilnUiSnapshotAcquire, capture_ms) == 32,
           "KilnUiSnapshotAcquire v1");
KUW_ASSERT(sizeof(KilnUiWorldValues) == 152 && offsetof(KilnUiWorldValues, unavailable_reason) == 16 &&
               offsetof(KilnUiWorldValues, publication) == 32 && offsetof(KilnUiWorldValues, local_capture_ms) == 56 &&
               offsetof(KilnUiWorldValues, local_player) == 64 && offsetof(KilnUiWorldValues, mana_max) == 88 &&
               offsetof(KilnUiWorldValues, x_fixed) == 96 && offsetof(KilnUiWorldValues, z_fixed) == 112 &&
               offsetof(KilnUiWorldValues, server_session) == 120 && offsetof(KilnUiWorldValues, server_hours) == 144,
           "KilnUiWorldValues v1");
KUW_ASSERT(sizeof(KilnUiInventoryRow) == 24 && offsetof(KilnUiInventoryRow, pide) == 20, "KilnUiInventoryRow v1");
KUW_ASSERT(sizeof(KilnUiDemandResult) == 24 && offsetof(KilnUiDemandResult, handle) == 16, "KilnUiDemandResult v1");
KUW_ASSERT(offsetof(KilnUiSnapshotApi, demand_add) == 8 && offsetof(KilnUiSnapshotApi, release) == 8 + 4 * sizeof(void*) &&
               sizeof(KilnUiSnapshotApi) == 8 + 5 * sizeof(void*),
           "KilnUiSnapshotApi v1");
#undef KUW_ASSERT

#ifdef __cplusplus
}
#endif
#endif
