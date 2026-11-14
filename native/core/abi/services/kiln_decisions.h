/* SPDX-License-Identifier: MIT
 * kiln.decisions v1 (server side, kiln-decisions.dll): decisions about things the game is about to do.
 *
 * A producer (a hook inside a game program, or a Pre adapter of another service) captures something the game is
 * about to do (an entity dying, a walk-over pickup, an equipment change, a build edit), holds it, and files a
 * request. One consumer (the managed host's dispatcher) reads the requests in Pre, lets the mods' handlers decide,
 * and answers; the producer then lets the action happen or not. Every request ends in exactly one outcome record
 * with a terminal status. No managed code and no tick-only service call ever runs inside a hook: hooks copy a
 * request and read a verdict, nothing else.
 *
 * Step windows. The core calls window_begin(epoch, step) after the last Pre subscriber and window_end(epoch, step)
 * after WaitWorldJobs, before the Post subscribers (kiln_window_clock.h, phases 2 and 4). No world job is live in
 * either. window_begin publishes the immutable metadata table the hooks of that step read; deadlines are checked
 * there. Without feature "tick.wait_jobs" the service reports unavailable.
 *
 * Identity. A request names entities by KilnEventEntityKey: the entity id plus what tells one life of that id from
 * the next (SpawnTime when the entity has one, the player session for player ids 1..1023) and life_serial, a
 * process-wide serial the metadata cache gives every newly seen life. life_serial is not saved and is not
 * EntityRef.Serial. An entity without a usable identity is never held (counter metadata_misses).
 *
 * Requests. At most 4096 are active and 8192 outcomes are kept. request_id is a nonzero u64 that only grows within
 * the process; (world_epoch, request_id) is the key, and an answer must also name the kind. Hook captures (kinds 1,
 * 2) and queued set decisions (18) have deadline_step = capture_step + 2: answers are taken in the next two Pres,
 * and at the window_begin of capture_step + 3 an unanswered hold is released with TIMEOUT_RELEASED. Client-input
 * adapters (16, 17) keep the one-Pre hold of kiln.playerstate: enqueue, dispatch and answer happen in the same Pre
 * (deadline_step = that step); an unanswered one is finished TIMEOUT_RELEASED at that step's window_end. A full
 * queue fails open (QUEUE_DROPPED), never holds.
 *
 * Batches (requests and outcomes alike). begin_* freezes what has arrived into one batch and returns its id and its
 * record count; begin again before ack returns the same batch. read_* copies records: capacity and *required count
 * records, stride is the caller's record size in bytes and must be at least sizeof the V1 record. A null buffer or
 * a too small capacity writes nothing, consumes nothing, sets *required and returns KILN_ERR_BUFFER_TOO_SMALL. A
 * successful read consumes nothing either; ack_* does, and only the delivery copies, never held state. Acking the
 * same batch again is accepted for two Pres. What arrives after begin belongs to the next batch.
 *
 * Answers are idempotent: answering an id that is already terminal returns its terminal status in the receipt and
 * does nothing else. detach releases every pending hold (CAPABILITY_RELEASED). World stop ends them (WORLD_ENDED).
 *
 * Threads. attach, detach, interest, enqueue_prepared, the batch calls, answer, resolve_entity, reevaluate_pickup:
 * the world thread in a world-safe phase (kiln_window_clock.h), KILN_ERR_WRONG_THREAD otherwise. supported_kinds:
 * any thread. window_begin and window_end are called by the core only.
 */
#pragma once
#include <stddef.h>
#include "../kiln_api.h"
#pragma pack(push, 8)
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_DECISIONS_SERVICE "kiln.decisions"
#define KILN_DECISIONS_VERSION 1

#define KILN_DECISIONS_MAX_ACTIVE 4096u
#define KILN_DECISIONS_MAX_OUTCOMES 8192u
#define KILN_DECISIONS_MAX_METADATA 8192u
#define KILN_DECISIONS_MAX_INTEREST 4096u /* entity ids of one interest call */

/* KilnEventEntityKey.flags */
#define KILN_EVENT_KEY_SPAWN_TIME 1u
#define KILN_EVENT_KEY_PLAYER_SESSION 2u
/* KilnDecisionRequestV1.flags */
#define KILN_DECISION_FLAG_HAS_POSITION 1u
#define KILN_DECISION_FLAG_DRY_RUN 2u
/* KilnDecisionOutcomeV1.status; Ok iff 1..3 */
#define KILN_DECISION_ALLOWED 1u
#define KILN_DECISION_PREVENTED 2u
#define KILN_DECISION_ALREADY_ALIVE 3u
#define KILN_DECISION_STALE_ENTITY 4u
#define KILN_DECISION_NO_POSITIVE_RANGE 5u
#define KILN_DECISION_WRITE_REFUSED 6u
#define KILN_DECISION_TIMEOUT_RELEASED 7u
#define KILN_DECISION_CAPABILITY_RELEASED 8u
#define KILN_DECISION_SOURCE_CHANGED 9u
#define KILN_DECISION_QUEUE_DROPPED 10u
#define KILN_DECISION_WORLD_ENDED 11u
/* KilnDecisionOutcomeV1.flags */
#define KILN_DECISION_OUTCOME_HEALTH_KNOWN 1u

#define KILN_DECISION_DEATH 1u
#define KILN_DECISION_WALKOVER 2u
#define KILN_DECISION_EQUIP_SINGLE 16u
#define KILN_DECISION_EQUIP_COLLECTION 17u
#define KILN_DECISION_EQUIP_SET 18u
#define KILN_DECISION_BUILD_PIECE 32u
#define KILN_DECISION_TERRAFORM_RESERVED 33u /* unsupported by managed V1 */
typedef struct KilnEventEntityKey {
    uint32_t entity, flags; /* flags: 1 SpawnTime present, 2 live player session */
    uint64_t spawn_time, life_serial, player_handle, joined_step;
} KilnEventEntityKey; /* 40 */
typedef struct KilnSetOpV1 {
    uint32_t size;
    uint8_t equipment_slot, kind; /* 0 Empty, 1 Instance, 2 WholeStack */
    uint16_t reserved;
    uint32_t item_entity, item_id, expected_count, reserved2;
    uint64_t instance_stamp, source_slot; /* stamp = zero-extended native u32, not EntityRef.Serial */
} KilnSetOpV1; /* 40 */
typedef struct KilnDeathRequestV1 {
    int32_t health, maximum_health;
    uint64_t effective_actor_states;
    uint32_t suggested_survival_health, reserved;
} KilnDeathRequestV1;
typedef struct KilnPickupRequestV1 {
    uint32_t item_id, raw_count, feedback, entry; /* entry 1 = range; 2 = interaction witness */
    double position[3]; /* request flag bit0 says valid; otherwise all zero */
} KilnPickupRequestV1;
typedef struct KilnSingleEquipRequestV1 {
    uint32_t operation, equipment_slot; /* operation 1 Equip, 2 Unequip */
    uint32_t action_index, input_version; /* UINT32_MAX index for queued Kiln request */
    KilnSetOpV1 selection;
} KilnSingleEquipRequestV1;
typedef struct KilnCollectionEquipRequestV1 {
    KilnEventEntityKey target_inventory;
    uint32_t target, input_version; /* target 0 ActiveEquipment, 1 Vanity */
    uint32_t action_index, reserved;
} KilnCollectionEquipRequestV1;
typedef struct KilnSetDecisionRequestV1 {
    uint64_t items_ticket, snapshot_token;
    uint32_t count, flags; /* flag 1 DryRun */
    KilnSetOpV1 entries[20];
} KilnSetDecisionRequestV1;
typedef struct KilnBuildRequestV1 {
    uint32_t edit_kind, item_id, material_item_id, placement_flag;
    double position[3]; float rotation[4]; uint64_t lookup_key;
    uint32_t target_entity, payload_bytes; uint8_t payload[256];
} KilnBuildRequestV1; /* reserved until piece replay gate passes; no Terraform payload */
typedef struct KilnDecisionRequestV1 {
    uint32_t size, version, kind, payload_bytes;
    uint64_t request_id, world_epoch, capture_step, deadline_step;
    KilnEventEntityKey subject, actor;
    uint64_t exposure_epoch;
    uint32_t flags, reserved; /* bit0 HasPosition; bit1 DryRun */
    union {
        KilnDeathRequestV1 death; KilnPickupRequestV1 pickup;
        KilnSingleEquipRequestV1 single; KilnCollectionEquipRequestV1 collection;
        KilnSetDecisionRequestV1 set; KilnBuildRequestV1 build;
        uint8_t bytes[1024];
    } payload;
} KilnDecisionRequestV1; /* 1168 */
typedef struct KilnDecisionInterestV1 {
    uint32_t size, kind, all_entities, reserved;
    uint64_t registry_revision;
    const uint32_t *entities; uint32_t entity_count, reserved2;
} KilnDecisionInterestV1;
typedef struct KilnDecisionAnswerV1 {
    uint32_t size, kind; uint64_t request_id, world_epoch;
    uint32_t allow; int32_t survival_health;
} KilnDecisionAnswerV1;
typedef struct KilnDecisionReceiptV1 {
    uint32_t size, accepted, terminal_status, reserved; char reason[192];
} KilnDecisionReceiptV1;
/* outcome status: 1 Allowed, 2 Prevented, 3 AlreadyAlive, 4 StaleEntity,
   5 NoPositiveRange, 6 WriteRefused, 7 TimeoutReleased, 8 CapabilityReleased,
   9 SourceChanged, 10 QueueDropped, 11 WorldEnded. Ok iff status 1..3. */
typedef struct KilnDecisionOutcomeV1 {
    uint32_t size, version, kind, status;
    uint64_t request_id, world_epoch, step;
    KilnEventEntityKey subject, actor;
    int32_t requested_health, health_before, applied_health;
    uint32_t flags; /* bit0 health_before/applied_health known */
    char reason[192];
} KilnDecisionOutcomeV1;

typedef struct KilnDecisionsApiV1 {
    KilnServiceHeader header;
    /* Bit n is set when kind n has a producer that is installed and on. Kind 33 is never set in v1. */
    uint64_t(KILN_CALL* supported_kinds)(void);
    /* Makes `dispatcher` the one consumer. KILN_ERR_UNAVAILABLE while another owner is attached. */
    KilnResult(KILN_CALL* attach)(KilnOwner dispatcher);
    /* Refreshes and returns the metadata row of one entity without a discovery scan. position may be null.
     * KILN_ERR_NOT_FOUND: no entity, or no usable identity (counted in metadata_misses). */
    KilnResult(KILN_CALL* resolve_entity)(uint32_t entity, KilnEventEntityKey* key, double position[3], uint32_t* has_position);
    /* Replaces the consumer's interest in one kind: all entities, or a copied list (at most
     * KILN_DECISIONS_MAX_INTEREST). all_entities 0 with entity_count 0 removes the interest; a producer with no
     * interest captures nothing. registry_revision is echoed in diagnostics only. */
    KilnResult(KILN_CALL* interest)(KilnOwner dispatcher, const KilnDecisionInterestV1* interest);
    /* Pre adapters only, kinds 16, 17, 18: copies the request and assigns request_id, world_epoch, capture_step and
     * deadline_step (the current step for 16 and 17, +2 for 18). size, version, kind and payload_bytes must agree.
     * KILN_ERR_BUFFER_TOO_SMALL when the queue is full: the caller lets the action run (fail open). */
    KilnResult(KILN_CALL* enqueue_prepared)(KilnOwner dispatcher, const KilnDecisionRequestV1* request, uint64_t* request_id);
    KilnResult(KILN_CALL* begin_requests)(KilnOwner dispatcher, uint64_t* batch, uint32_t* records);
    KilnResult(KILN_CALL* read_requests)(KilnOwner dispatcher, uint64_t batch, KilnDecisionRequestV1* out, uint32_t capacity,
                                         uint32_t stride, uint32_t* required);
    KilnResult(KILN_CALL* ack_requests)(KilnOwner dispatcher, uint64_t batch);
    /* Answers one request. receipt->accepted is 1 when this call decided it, 0 when it was terminal already or is
     * unknown; terminal_status is its terminal status (0 when unknown, with KILN_ERR_NOT_FOUND). */
    KilnResult(KILN_CALL* answer)(KilnOwner dispatcher, const KilnDecisionAnswerV1* answer, KilnDecisionReceiptV1* receipt);
    KilnResult(KILN_CALL* begin_outcomes)(KilnOwner dispatcher, uint64_t* batch, uint32_t* records);
    KilnResult(KILN_CALL* read_outcomes)(KilnOwner dispatcher, uint64_t batch, KilnDecisionOutcomeV1* out, uint32_t capacity,
                                         uint32_t stride, uint32_t* required);
    KilnResult(KILN_CALL* ack_outcomes)(KilnOwner dispatcher, uint64_t batch);
    /* Forgets what the walk-over producer remembers about (drop, receiver) so the next exposure asks again.
     * 0 for either means "any". KILN_ERR_UNAVAILABLE without the pickup producer. */
    KilnResult(KILN_CALL* reevaluate_pickup)(uint32_t drop, uint32_t receiver, uint32_t* exposures_reset);
    /* Releases every pending hold and the consumer's interests. */
    KilnResult(KILN_CALL* detach)(KilnOwner dispatcher);
    /* Core only: after all Pre subscribers / after WaitWorldJobs, before the Post subscribers. */
    void(KILN_CALL* window_begin)(uint64_t world_epoch, uint64_t step);
    void(KILN_CALL* window_end)(uint64_t world_epoch, uint64_t step);
} KilnDecisionsApiV1;

#ifdef __cplusplus
}
static_assert(sizeof(KilnEventEntityKey) == 40, "KilnEventEntityKey is 40 bytes");
static_assert(sizeof(KilnSetOpV1) == 40, "KilnSetOpV1 is 40 bytes");
static_assert(offsetof(KilnDecisionRequestV1, payload) == 144, "the request payload starts at 144");
static_assert(sizeof(KilnDecisionRequestV1) == 1168, "KilnDecisionRequestV1 is 1168 bytes");
static_assert(sizeof(KilnSetDecisionRequestV1) == 824, "KilnSetDecisionRequestV1 is 824 bytes");
static_assert(sizeof(KilnBuildRequestV1) <= 1024, "a build request fits the payload");
static_assert(sizeof(KilnDecisionOutcomeV1) == 328, "KilnDecisionOutcomeV1 is 328 bytes");
#endif
#pragma pack(pop)
