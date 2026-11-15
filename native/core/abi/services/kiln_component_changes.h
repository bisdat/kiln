/* SPDX-License-Identifier: MIT
 * kiln.component_changes v1 (server side, kiln-decisions.dll): net changes of named components of watched entities.
 *
 * A watch names entities (or "all live players") and components (Health, Stamina, Mana, Fuel, Inventory,
 * SlotSelection). At window_begin the service copies the watched components (the baseline); at window_end, when no
 * world job is live, it compares and queues one record per (entity, component) that differs, with the bytes before
 * and after. It is a net change per step, not a feed of every store. Native writers that change a component
 * themselves call record_write so the record can say Kiln (2) or Mixed (3) instead of Unknown (0).
 *
 * Budgets: 32 watches, 4096 (entity, component) pairs, 128 explicit roots per watch. A watch over budget is created
 * suspended (status 1) and says so; it never silently covers less than it claims.
 *
 * Events are read with the same freeze / read / ack batch rules as kiln.decisions (kiln_decisions.h).
 * Threads: record_write on any thread that legally writes the component; everything else on the world thread in a
 * world-safe phase (kiln_window_clock.h). window_begin and window_end are called by the core only.
 */
#pragma once
#include "kiln_decisions.h"
#pragma pack(push, 8)
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_COMPONENT_CHANGES_SERVICE "kiln.component_changes"
#define KILN_COMPONENT_CHANGES_VERSION 1

/* component mask: Health1 Stamina2 Mana4 Fuel8 Inventory16 SlotSelection32. */
typedef struct KilnComponentWatchRequestV1 {
 uint32_t size,scope,components,expansion; /* scope 0 Entities / 1 live Players; expansion 1 linked inventory/items */
 const uint32_t *entities; uint32_t entity_count,reserved; /* copied, max128 explicit roots */
} KilnComponentWatchRequestV1;
typedef struct KilnComponentWatchStatusV1 {
 uint32_t size,status,pairs,limit; /* status 0 Ready,1 SuspendedBudget,2 SuspendedMetadata,3 Disposed */
 uint64_t watch_id,last_complete_step; char reason[192];
} KilnComponentWatchStatusV1;
typedef struct KilnComponentChangeV1 {
 uint32_t size,version,kind,component; /* kind1; component 1..6 in mask order */
 uint64_t world_epoch,step; KilnEventEntityKey entity;
 uint32_t before_bytes,after_bytes,origin,flags; /* flags1 before-present,2 after-present,4 token-truncated */
 uint32_t root_count,watch_count,token_count,reserved;
 uint32_t roots[128]; uint64_t watches[32],tokens[32];
 uint8_t before[256],after[256]; char source_mod[128];
} KilnComponentChangeV1;
typedef struct KilnKnownWriteV1 {
 uint32_t size,component; uint64_t world_epoch,step,operation_token;
 KilnEventEntityKey entity; const uint8_t *before,*after;
 uint32_t bytes,reserved; KilnStr source_mod; /* copied; >127 UTF8 bytes -> Unknown provenance */
} KilnKnownWriteV1;
typedef struct KilnComponentChangesApiV1 {
 KilnServiceHeader header;
 KilnResult(KILN_CALL *attach)(KilnOwner dispatcher);
 KilnResult(KILN_CALL *detach)(KilnOwner dispatcher);
 KilnResult(KILN_CALL *watch)(KilnOwner,const KilnComponentWatchRequestV1*,KilnComponentWatchStatusV1*);
 KilnResult(KILN_CALL *resume)(KilnOwner,uint64_t watch_id,KilnComponentWatchStatusV1*);
 KilnResult(KILN_CALL *status)(uint64_t watch_id,KilnComponentWatchStatusV1*);
 KilnResult(KILN_CALL *remove)(KilnOwner,uint64_t watch_id);
 KilnResult(KILN_CALL *release_owner)(KilnOwner);
 KilnResult(KILN_CALL *begin_events)(KilnOwner dispatcher,uint64_t *batch,uint32_t *records);
 KilnResult(KILN_CALL *read_events)(KilnOwner,uint64_t batch,KilnComponentChangeV1*,uint32_t capacity,uint32_t stride,uint32_t *required);
 KilnResult(KILN_CALL *ack_events)(KilnOwner,uint64_t batch);
 /* native service owners only; data valid during call, copied; any legal component writer thread */
 KilnResult(KILN_CALL *record_write)(KilnOwner,const KilnKnownWriteV1*);
 /* core-internal only: after all Pre / after WaitWorldJobs before ordinary Post */
 void(KILN_CALL *window_begin)(uint64_t world_epoch,uint64_t step);
 void(KILN_CALL *window_end)(uint64_t world_epoch,uint64_t step);
} KilnComponentChangesApiV1;

#ifdef __cplusplus
}
static_assert(sizeof(KilnComponentChangeV1) <= 2048, "KilnComponentChangeV1 fits 2048 bytes");
#endif
#pragma pack(pop)
