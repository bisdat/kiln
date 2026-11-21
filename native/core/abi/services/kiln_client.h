/* SPDX-License-Identifier: MIT
 * kiln.client v1: what a mod needs to know about the game client it runs in (in-core service, both sides).
 *  - state: side, session (main menu / hosting / joined a remote world), the local player's entity. Any thread.
 *  - the client world's tick (Pre/Post on the game's main thread, ~60 Hz while in game) and a task queue to it.
 * On a dedicated server the service is available too: side KILN_SIDE_SERVER, session KILN_SESSION_DEDICATED, no
 * client world, local_player 0. See docs/kiln/client.md. */
#ifndef KILN_CLIENT_H
#define KILN_CLIENT_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_CLIENT_SERVICE "kiln.client"
#define KILN_CLIENT_VERSION 1u

enum {
    KILN_CLIENT_LOCAL_PLAYER = 1u << 0, /* local_player is known (LocalPlayerData resolved in the client world) */
    KILN_CLIENT_LIFECYCLE = 1u << 1,    /* world stops are seen (feature world.lifecycle) */
};

typedef struct KilnClientState {
    uint32_t size;            /* set by the caller: sizeof(KilnClientState) */
    uint32_t side;            /* KILN_SIDE_* */
    uint32_t session;         /* KILN_SESSION_*: NONE (menu, loading), HOST (single player / co-op host), REMOTE */
    uint32_t generation;      /* changes whenever a world starts or stops */
    KilnEntity local_player;  /* the local player's entity, 0 when none. Ids are shared by the client and server
                                 worlds on a host, so this id also works with the server world's services there. */
    uint32_t flags;           /* KILN_CLIENT_* */
    uint32_t client_session;  /* client worlds since boot (each joined/hosted game is one) */
    uint32_t server_session;  /* server worlds since boot (each hosted game is one) */
    uint64_t client_steps;    /* client-world steps since boot */
    uint64_t server_steps;    /* server-world steps since boot */
} KilnClientState;

/* Building input (since 0.2.0-dev.30.3): one building action at a time for the whole client, emitted through the local
 * player's ordinary input path. Bound on game client 26430625; the capability row client.building_input says whether
 * this build has it. Kiln hooks the input path at the first request, never before. See
 * docs/kiln/services/client-building-input.md. */
enum {
    KILN_BUILDING_ACTION_SELECT = 0,    /* select item_id (and material_item_id) in actionbar slot `slot` */
    KILN_BUILDING_ACTION_PLACE = 1,     /* the primary building action at the transform */
    KILN_BUILDING_ACTION_REMOVE = 2,    /* the secondary building action at the transform */
    KILN_BUILDING_ACTION_UNDO = 3,      /* the game's own building undo */
    KILN_BUILDING_ACTION_DISMANTLE = 4, /* dismantle the exact prop target_entity_id at the transform (held 1.2 s) */
};
enum {
    KILN_BUILDING_INPUT_UNKNOWN = 0,    /* not the caller's latest request */
    KILN_BUILDING_INPUT_QUEUED = 1,     /* waiting for the input path (at most 5 s) */
    KILN_BUILDING_INPUT_PRESSED = 2,    /* the action is held in the input; released at a later input version */
    KILN_BUILDING_INPUT_DISPATCHED = 3, /* emitted as input: not the world's acknowledgement */
    KILN_BUILDING_INPUT_TIMEOUT = 4,    /* not emitted within 5 s (a pressed one is still released) */
    KILN_BUILDING_INPUT_CANCELLED = 5,  /* cancelled; its owner, its session or the producer ended; or the input faulted */
};
#define KILN_CLIENT_ERR_BUILDING_BUSY 200  /* a request (of any owner) is queued or pressed */
#define KILN_CLIENT_ERR_BUILDING_STALE 201 /* no client world is live, or expected_session is not its session */

typedef struct KilnBuildingInput {
    uint32_t size;             /* set by the caller: sizeof(KilnBuildingInput) */
    uint32_t action;           /* KILN_BUILDING_ACTION_* */
    KilnEntity player;         /* whose ClientPlayerInput carries it: the local player (KilnClientState.local_player) */
    uint32_t item_id;          /* SELECT: the building item, required */
    uint32_t material_item_id; /* SELECT: terrain and default building material, 0 = leave the game's choice */
    uint32_t slot;             /* SELECT: the actionbar slot, 0..255 */
    uint32_t target_entity_id; /* DISMANTLE: the exact prop, required; 0 for every other action */
    uint32_t reserved;         /* 0 */
    double position[3];        /* PLACE, REMOVE, DISMANTLE: world position in metres */
    double rotation[4];        /* PLACE, REMOVE, DISMANTLE: quaternion x y z w, normalized here */
    double scale[3];           /* PLACE, REMOVE, DISMANTLE: each in (0, 1024] */
} KilnBuildingInput;

typedef struct KilnClientApi {
    KilnServiceHeader header; /* { sizeof(KilnClientApi), KILN_CLIENT_VERSION } */
    KilnResult(KILN_CALL* state)(KilnClientState* out);
    /* Pre/Post of the client world's steps, on the main thread (KILN_TICK_PRE / KILN_TICK_POST). The client world
     * exists while in game (hosting or joined); in the main menu nothing is called. Remove with unsubscribe. */
    KilnResult(KILN_CALL* subscribe_tick)(KilnOwner owner, int32_t phase, KilnTickFn fn, void* user, KilnHandle* out);
    KilnResult(KILN_CALL* unsubscribe)(KilnHandle handle);
    /* Runs `fn` on the main thread before the client world's next Pre (dropped with the owner). */
    KilnResult(KILN_CALL* post)(KilnOwner owner, KilnTaskFn fn, void* user);
    bool(KILN_CALL* on_client_thread)(void);
    /* The client world, 0 when none. Game memory: main thread only, inside a client tick. */
    uintptr_t(KILN_CALL* client_world)(void);
    /* Building input (see KilnBuildingInput), since 0.2.0-dev.30.3: appended at 56, check header.size >= 80 before
     * calling. Any thread; none of them waits for the game (the first submit of the process may take a few milliseconds:
     * it hooks the input path).
     * building_input_submit: queues one request for the live client session (`expected_session` 0, or
     * KilnClientState.client_session). *request_id: its id, never 0; a newer request replaces it as the latest.
     * KILN_ERR_UNAVAILABLE: a dedicated server, safe idle, no input producer on this game build, or hooking the input path
     * failed at the first request (the capability row client.building_input says why); KILN_ERR_INVALID_ARGUMENT: owner 0, a short record or a field outside its rule;
     * KILN_ERR_REVOKED: a revoked owner; KILN_CLIENT_ERR_BUILDING_STALE, KILN_CLIENT_ERR_BUILDING_BUSY.
     * building_input_status: *state = KILN_BUILDING_INPUT_*, a copy; UNKNOWN for any id but the caller's latest.
     * building_input_cancel: *cancelled = 1 for the caller's latest id. A queued request is cancelled, a pressed one is
     * released at the next input; a dispatched one stays dispatched. Nothing confirms that the world built anything:
     * the server still checks permissions, resources, range and placement as for the player's own input. */
    KilnResult(KILN_CALL* building_input_submit)(KilnOwner owner, uint64_t expected_session,
                                                 const KilnBuildingInput* input, uint32_t* request_id);
    KilnResult(KILN_CALL* building_input_status)(KilnOwner owner, uint32_t request_id, uint32_t* state);
    KilnResult(KILN_CALL* building_input_cancel)(KilnOwner owner, uint32_t request_id, uint32_t* cancelled);
} KilnClientApi;

#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
static_assert(sizeof(KilnBuildingInput) == 112, "KilnBuildingInput is 112 bytes");
static_assert(offsetof(KilnBuildingInput, target_entity_id) == 24 && offsetof(KilnBuildingInput, position) == 32 &&
                  offsetof(KilnBuildingInput, rotation) == 56 && offsetof(KilnBuildingInput, scale) == 88,
              "KilnBuildingInput layout");
static_assert(offsetof(KilnClientApi, building_input_submit) == 56, "building_input_submit is appended at 56");
static_assert(offsetof(KilnClientApi, building_input_status) == 64, "building_input_status is appended at 64");
static_assert(offsetof(KilnClientApi, building_input_cancel) == 72, "building_input_cancel is appended at 72");
static_assert(sizeof(KilnClientApi) == 80, "KilnClientApi is 80 bytes");
#else
_Static_assert(sizeof(KilnBuildingInput) == 112, "KilnBuildingInput is 112 bytes");
_Static_assert(offsetof(KilnBuildingInput, target_entity_id) == 24 && offsetof(KilnBuildingInput, position) == 32 &&
                   offsetof(KilnBuildingInput, rotation) == 56 && offsetof(KilnBuildingInput, scale) == 88,
               "KilnBuildingInput layout");
_Static_assert(offsetof(KilnClientApi, building_input_submit) == 56, "building_input_submit is appended at 56");
_Static_assert(offsetof(KilnClientApi, building_input_status) == 64, "building_input_status is appended at 64");
_Static_assert(offsetof(KilnClientApi, building_input_cancel) == 72, "building_input_cancel is appended at 72");
_Static_assert(sizeof(KilnClientApi) == 80, "KilnClientApi is 80 bytes");
#endif
#endif
