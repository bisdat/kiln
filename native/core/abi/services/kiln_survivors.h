// SPDX-License-Identifier: MIT
/* kiln.survivors (kiln-quests.dll, next to kiln.knowledge): the game's roster survivors (the NPCs a player rescues and
 * summons with a tool) for mods: read the roster with live state, recruit a survivor for the world, give its summon tool,
 * summon or move it into a base, assign and unassign its bed. New table since 0.2.0-dev.31 (version 1).
 *
 * Ids: a survivor is its roster id, the game's NpcId: FNV-1a-32 of the NPC template's 16 GUID bytes (the same value as
 * kiln.creatures' npc_command and Kiln.Creatures' Villager.NpcId). Never persist an entity id: re-resolve by NpcId.
 *
 * Threads:
 *  - get, count, at, of_entity, bed_occupant: the server world's tick thread (KILN_ERR_WRONG_THREAD elsewhere).
 *  - status, submit, result, progress, events: any thread. submit copies and checks the request's shape at once
 *    (KILN_ERR_INVALID_ARGUMENT); the world, entity and authorization checks run on the tick thread when the request is
 *    taken (Pre), and again right before the write.
 *
 * Requests (submit): RECRUIT, SUMMON, ASSIGN_BED, UNASSIGN_BED. Each has a 30 s budget from acceptance (at most 5 s of it
 * queued) and never runs in a later world session. A request ends in exactly one terminal result (result(): NOT_FOUND
 * while it runs, then the result for 10 minutes, the last 256 kept). Kiln never takes back what a request already did:
 * a request that wrote something and then could not finish ends PARTIAL with the exact committed effects, never with a
 * rollback. A child step (a knowledge grant, an item transaction) is never cancelled: when the budget runs out while one
 * is in flight, the request drains (progress state DRAINING) until it settles, then ends with what really happened.
 *
 *  RECRUIT: the survivor's unlock knowledge (a plain world Bool in the game's roster) is granted for the world, then
 *    the game's NPC registry must confirm the unlock (10 s or 180 world updates), then, with KILN_SURV_GIVE_TOOL, one
 *    summon tool goes to "recipient" (a player in game, checked at acceptance and again before the give; a slot reused
 *    by another player ends it). Already unlocked: no knowledge write (NO_CHANGE without a tool). A roster entry whose
 *    unlock is a compound query is refused (UnsupportedUnlockRequirement).
 *  SUMMON: "base_id" must be a loaded, not abandoned base and "position" inside its build zone (the caller picks a
 *    point on the ground; Kiln does not search). Locked survivors are recruited first (no tool). An NPC already in the
 *    world is moved with the game's Reset (teleport, home = position); otherwise its template is spawned there. Done
 *    when the survivor is unlocked, exactly one live entity, home within 0.05 m and position within 1 m of the request
 *    (a new entity must also live 3 s and two updates).
 *  ASSIGN_BED / UNASSIGN_BED: both ends of the game's relation (the NPC's bed and the bed's NPC) are written in one Kiln
 *    program run with tracked writes (the save and replication see them); the NPC's old bed and the bed's previous NPC
 *    are cleared (displaced_*). Confirmed by a readback one update after the write. authorization: PLAYER_PERMISSIONS
 *    needs a player in game as "actor" with the base-edit permission, and every bed touched inside a loaded, not
 *    abandoned base where the zone rules let that player build; TRUSTED_SERVER needs actor 0 and skips only those player
 *    checks. A bed farther from the NPC's home than its own bed radius is refused (BedOutOfHomeRange).
 *
 * Events (events()): RECRUITED (registry confirmed), TOOL_GIVEN, SUMMONED, ASSIGNMENT_CHANGED. request 0 = a change the
 * game made itself (a player's summon tool, a destroyed bed, the game's bed upkeep), seen by a comparison of the live
 * roster's beds at most once a second. The last 4096 are kept.
 *
 * Docs: docs/kiln/services/survivors.md. Engine facts: dossier chapter 18, docs/re/quests.md. */
#pragma once
#include "../kiln_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_SURVIVORS_SERVICE "kiln.survivors"
#define KILN_SURVIVORS_VERSION 1u
#define KILN_SURVIVOR_DEADLINE_MS 30000u
#define KILN_SURVIVOR_QUEUE_MS 5000u
#define KILN_SURVIVOR_NAME_MAX 96u

/* KilnSurvivorRequest.op */
enum { KILN_SURV_OP_RECRUIT = 1, KILN_SURV_OP_SUMMON = 2, KILN_SURV_OP_ASSIGN_BED = 3, KILN_SURV_OP_UNASSIGN_BED = 4 };
/* KilnSurvivorRequest.flags (RECRUIT only; other bits and other ops: INVALID_ARGUMENT) */
enum { KILN_SURV_GIVE_TOOL = 1u };
/* KilnSurvivorRequest.authorization (ASSIGN_BED / UNASSIGN_BED; 0 on other ops) */
enum { KILN_SURV_AUTH_PLAYER_PERMISSIONS = 0, KILN_SURV_AUTH_TRUSTED_SERVER = 1 };
/* KilnSurvivorResult.status */
enum {
    KILN_SURV_DONE = 0,        /* everything requested happened and was confirmed */
    KILN_SURV_NO_CHANGE = 1,   /* it already was so: nothing written */
    KILN_SURV_REFUSED = 2,     /* refused before anything was written (reason) */
    KILN_SURV_TIMED_OUT = 3,   /* the budget ran out before anything was written */
    KILN_SURV_PARTIAL = 4,     /* something was written (committed_effects), then it could not finish (reason) */
    KILN_SURV_WORLD_ENDED = 5, /* the world stopped; committed_effects says what was written before */
};
/* committed_effects bits. REGISTRY_CONFIRMED is an observation, the others are writes. */
enum {
    KILN_SURV_KNOWLEDGE_WRITTEN = 1u, KILN_SURV_REGISTRY_CONFIRMED = 2u, KILN_SURV_TOOL_GIVEN = 4u,
    KILN_SURV_ENTITY_CREATED = 8u, KILN_SURV_HOME_MOVED = 16u, KILN_SURV_BED_CHANGED = 32u,
};
/* KilnSurvivorProgress.state */
enum { KILN_SURV_QUEUED = 0, KILN_SURV_APPLYING = 1, KILN_SURV_VERIFYING = 2, KILN_SURV_DRAINING = 3, KILN_SURV_TERMINAL = 4 };
/* KilnSurvivorEvent.kind */
enum { KILN_SURV_EV_RECRUITED = 1, KILN_SURV_EV_TOOL_GIVEN = 2, KILN_SURV_EV_SUMMONED = 3, KILN_SURV_EV_ASSIGNMENT_CHANGED = 4 };
/* KilnSurvivorState.flags */
enum {
    KILN_SURV_STATE_IN_WORLD = 1u,  /* entity is a live NPC of this survivor */
    KILN_SURV_STATE_HAS_HOME = 2u,  /* home is valid */
    KILN_SURV_STATE_HAS_BASE = 4u,  /* base_id is valid: the base whose build zone holds home */
};

typedef struct KilnSurvivorRequest {
    uint32_t size;          /* sizeof(KilnSurvivorRequest) */
    uint32_t op;            /* KILN_SURV_OP_* */
    uint32_t npc_id;        /* the survivor (NpcId), not 0 */
    uint32_t flags;         /* KILN_SURV_GIVE_TOOL (RECRUIT) */
    uint32_t authorization; /* KILN_SURV_AUTH_* (ASSIGN_BED / UNASSIGN_BED) */
    uint32_t reserved;      /* 0 */
    KilnEntity actor;       /* PLAYER_PERMISSIONS: the player acting (not 0); TRUSTED_SERVER: 0 */
    KilnEntity recipient;   /* RECRUIT with GIVE_TOOL: the player who gets the tool (not 0); else 0 */
    KilnEntity bed;         /* ASSIGN_BED: the bed entity (not 0); else 0 */
    uint32_t reserved1;     /* 0 */
    uint8_t base_id[16];    /* SUMMON: the base (the altar's BaseId); else zero */
    KilnVec3 position;      /* SUMMON: where it stands (metres, on the ground) */
    float yaw_degrees;      /* SUMMON: facing, about +Y */
    uint32_t reserved2;     /* 0 */
} KilnSurvivorRequest;      /* 88 bytes */

typedef struct KilnSurvivorState {
    uint32_t size;          /* sizeof(KilnSurvivorState) */
    uint32_t npc_id;
    KilnEntity entity;      /* the live NPC, 0 when it is not in the world */
    KilnEntity bed;         /* its bed (the NPC's side of the relation), 0 none */
    uint32_t summon_item;   /* the summon tool's ItemId, 0 none */
    uint32_t unlocked;      /* 1 unlocked for the world, 0 locked, 0xff unknown */
    uint32_t flags;         /* KILN_SURV_STATE_* */
    uint32_t index;         /* roster index */
    uint8_t base_id[16];    /* HAS_BASE: the base whose zone holds home */
    KilnVec3 home;          /* HAS_HOME: the point it lives around */
    char name[KILN_SURVIVOR_NAME_MAX]; /* the roster's debug name, NUL-terminated */
} KilnSurvivorState;        /* 168 bytes */

typedef struct KilnSurvivorResult {
    uint32_t size;          /* sizeof(KilnSurvivorResult) */
    uint32_t status;        /* KILN_SURV_DONE .. */
    uint64_t request;
    uint64_t step;          /* server-world step it ended in */
    uint32_t committed_effects; /* KILN_SURV_* effect bits */
    uint32_t after_known;   /* 1: after is a readback at the end; 0: the last confirmed state */
    KilnEntity displaced_entity; /* ASSIGN_BED: the NPC that lost the bed, 0 none */
    uint32_t displaced_npc;      /* its NpcId */
    uint32_t op;            /* KILN_SURV_OP_* of the request */
    uint32_t steps;         /* world updates from acceptance to the end */
    KilnSurvivorState before, after;
    char reason[160];       /* "" for DONE / NO_CHANGE, else "Code: words" (Code as in docs) */
} KilnSurvivorResult;       /* 544 bytes */

typedef struct KilnSurvivorProgress {
    uint32_t size;          /* sizeof(KilnSurvivorProgress) */
    uint32_t state;         /* KILN_SURV_QUEUED .. TERMINAL */
    uint64_t request;
    uint64_t elapsed_ms;    /* since acceptance */
    uint32_t committed_effects;
    uint32_t in_flight;     /* child steps not settled yet */
    char reason[160];       /* what it waits for */
} KilnSurvivorProgress;     /* 192 bytes */

typedef struct KilnSurvivorEvent {
    uint32_t size;          /* sizeof(KilnSurvivorEvent) */
    uint32_t kind;          /* KILN_SURV_EV_* */
    uint64_t sequence;      /* 1, 2, 3, ... */
    uint64_t request;       /* 0: the game made the change */
    uint64_t step;
    KilnSurvivorState before, after;
} KilnSurvivorEvent;        /* 368 bytes */

typedef struct KilnSurvivorsStatus {
    uint32_t size;          /* sizeof(KilnSurvivorsStatus) */
    uint8_t read;           /* the roster and live state can be read (survivors.read) */
    uint8_t recruit;        /* survivors.recruit */
    uint8_t summon;         /* survivors.summon */
    uint8_t assign;         /* survivors.assign */
    uint32_t pending;       /* requests not ended */
    uint32_t reserved;
    uint64_t submitted, ended, events;
    char detail[160];       /* why something is off; NUL-terminated */
} KilnSurvivorsStatus;      /* 200 bytes */

typedef struct KilnSurvivorsApi {
    KilnServiceHeader header; /* { sizeof(KilnSurvivorsApi), KILN_SURVIVORS_VERSION } */

    /* any thread */
    KilnResult(KILN_CALL* status)(KilnSurvivorsStatus* out);
    /* ---- reads (tick) ---- */
    /* Roster entries (count(): 0 before the world's catalog is built). */
    uint32_t(KILN_CALL* count)(void);
    KilnResult(KILN_CALL* at)(uint32_t index, KilnSurvivorState* out);
    /* By NpcId. KILN_ERR_NOT_FOUND: not a roster entry. */
    KilnResult(KILN_CALL* get)(uint32_t npc_id, KilnSurvivorState* out);
    /* The NpcId of a live NPC entity. KILN_ERR_NOT_FOUND: not a roster NPC. */
    KilnResult(KILN_CALL* of_entity)(KilnEntity npc, uint32_t* npc_id);
    /* The NpcId a bed is assigned to (the bed's side), 0 when free. KILN_ERR_NOT_FOUND: not a bed. */
    KilnResult(KILN_CALL* bed_occupant)(KilnEntity bed, uint32_t* npc_id);
    /* The base whose build zone holds `position`: its id (for SUMMON) and its altar; *abandoned 1 when abandoned.
     * KILN_ERR_NOT_FOUND: no loaded base zone holds it. */
    KilnResult(KILN_CALL* base_at)(KilnVec3 position, uint8_t base_id[16], KilnEntity* altar, uint32_t* abandoned);
    /* ---- requests (any thread) ---- */
    /* KILN_ERR_UNAVAILABLE: the operation's capability is off (status() says why). */
    KilnResult(KILN_CALL* submit)(KilnOwner owner, const KilnSurvivorRequest* request, uint64_t* out_request);
    /* KILN_ERR_NOT_FOUND while it runs, and after it was forgotten (10 minutes, the last 256). */
    KilnResult(KILN_CALL* result)(uint64_t request, KilnSurvivorResult* out);
    KilnResult(KILN_CALL* progress)(uint64_t request, KilnSurvivorProgress* out);
    /* Events with sequence > after, oldest first; *out_count written. */
    KilnResult(KILN_CALL* events)(uint64_t after, KilnSurvivorEvent* out, uint32_t capacity, uint32_t* out_count);
} KilnSurvivorsApi;

#ifdef __cplusplus
}
static_assert(sizeof(KilnSurvivorRequest) == 88, "KilnSurvivorRequest layout");
static_assert(sizeof(KilnSurvivorState) == 168, "KilnSurvivorState layout");
static_assert(sizeof(KilnSurvivorResult) == 544, "KilnSurvivorResult layout");
static_assert(sizeof(KilnSurvivorProgress) == 192, "KilnSurvivorProgress layout");
static_assert(sizeof(KilnSurvivorEvent) == 368, "KilnSurvivorEvent layout");
static_assert(sizeof(KilnSurvivorsStatus) == 200, "KilnSurvivorsStatus layout");
static_assert(sizeof(KilnSurvivorsApi) == 8 + 11 * sizeof(void*), "KilnSurvivorsApi layout");
#endif
