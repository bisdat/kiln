/* SPDX-License-Identifier: MIT
 * kiln.client.crosshair v1 (since 0.2.0-dev.31.13, mod API 0.2.22): a read-only copy of the game client's own
 * crosshair hit, the scene trace its cursor program (client_cursor) runs from the local camera along its forward axis
 * every client step. It is the hit the stock HUD and cursor use: Kiln does no raycast of its own.
 * docs/kiln/services/client-crosshair.md; the game side: docs/re/client.md, "The gameplay crosshair trace".
 *
 *  - get: the newest sample. Any thread (the client tick thread, a mod's UI thread, a worker); never waits, never calls
 *    into the game. The game's trace is copied by a mid hook that Kiln adds at the first get of the process (a game
 *    without a mod that reads it never has the hook), so the first get answers KILN_CROSSHAIR_REASON_NO_SAMPLE. An
 *    install refused because some thread could not be checked is tried again by a get at least a second later (three
 *    attempts in all); until then get keeps answering NO_SAMPLE.
 *  - The capability row client.crosshair says whether it works here: the game client, kiln.client.crosshair started,
 *    the sigdb feature client.crosshair_trace resolved on this build, and the hook not failed. Off: get answers
 *    KILN_ERR_UNAVAILABLE.
 *
 * A sample is values only (no game pointer). The trace's reach is the held item's (keen ItemInfo.equipment.maxDistance)
 * clamped by the game to 7..31 m and measured from the trace's start, which the game puts on the camera's forward axis
 * (it is not the eye). Freshness: `sequence` grows by one per copied trace, `client_session` and `client_step` name the
 * client world and its step (KilnClientState.client_session / client_steps), `age_ns` is the sample's age at this get.
 * The game stops tracing while its cursor program does not run (a loading screen, the main menu, and wherever else
 * the program skips the local player): the newest sample then ages past KILN_CROSSHAIR_FRESH_NS and get says STALE.
 */
#ifndef KILN_CLIENT_CROSSHAIR_H
#define KILN_CLIENT_CROSSHAIR_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_CLIENT_CROSSHAIR_SERVICE "kiln.client.crosshair"
#define KILN_CLIENT_CROSSHAIR_VERSION 1u

/* A sample older than this (at get) is STALE: about 15 client steps at 60 Hz. */
#define KILN_CROSSHAIR_FRESH_NS 250000000ull

/* KilnCrosshairHit.flags: which fields hold a value. Without HIT nothing else is set. */
enum {
    KILN_CROSSHAIR_HIT = 1u << 0,      /* the game's trace hit something within its reach (status 0) */
    KILN_CROSSHAIR_POSITION = 1u << 1, /* position: the hit point, world metres (the game's float, moved 1 cm) */
    KILN_CROSSHAIR_NORMAL = 1u << 2,   /* normal: the surface normal there, unit length (within 1e-2) */
    KILN_CROSSHAIR_DISTANCE = 1u << 3, /* distance: metres from the trace's start to the hit */
    KILN_CROSSHAIR_ENTITY = 1u << 4,   /* entity: the game's entity of the hit (0 and unset for terrain) */
    KILN_CROSSHAIR_MATERIAL = 1u << 5, /* material: the voxel material id the game reports for the hit surface */
};

/* get()'s *out_reason. NONE: a fresh sample of the live client session. */
enum {
    KILN_CROSSHAIR_REASON_NONE = 0,
    KILN_CROSSHAIR_REASON_NO_SAMPLE = 1, /* nothing in this client session yet (out->sequence 0, the rest zero) */
    KILN_CROSSHAIR_REASON_BUSY = 2,      /* a copy was being published at this moment: ask again (out zeroed) */
    KILN_CROSSHAIR_REASON_STALE = 3,     /* out holds the newest sample, older than KILN_CROSSHAIR_FRESH_NS */
    KILN_CROSSHAIR_REASON_FAULT = 4,     /* reading the game's result faulted: no samples until the next client session */
};

/* The game's own result byte (KilnCrosshairHit.status), as it wrote it on client 26430625. */
enum {
    KILN_CROSSHAIR_STATUS_HIT = 0,       /* hit */
    KILN_CROSSHAIR_STATUS_NO_CAMERA = 21, /* no usable camera direction (a degenerate rotation) */
    KILN_CROSSHAIR_STATUS_NOTHING = 31,  /* nothing within reach */
};

typedef struct KilnCrosshairHit {
    uint32_t size;           /* set by the caller: sizeof(KilnCrosshairHit) */
    uint32_t flags;          /* KILN_CROSSHAIR_* */
    uint32_t status;         /* the game's result byte, KILN_CROSSHAIR_STATUS_* (another value: kept as it came) */
    KilnEntity entity;       /* ENTITY: the hit entity; 0 when none */
    uint32_t material;       /* MATERIAL: keen VoxelMaterialId (0..255); 0 when none */
    uint32_t client_session; /* the client world session of the sample (KilnClientState.client_session) */
    uint64_t sequence;       /* traces copied since the game started; never 0 for a sample */
    uint64_t client_step;    /* the client world's step count at the sample (KilnClientState.client_steps) */
    uint64_t age_ns;         /* the sample's age at this get, monotonic nanoseconds */
    double position[3];      /* POSITION: world metres (x, y up, z) */
    float normal[3];         /* NORMAL */
    float distance;          /* DISTANCE: metres */
} KilnCrosshairHit;

typedef struct KilnClientCrosshairApi {
    KilnServiceHeader header; /* { sizeof(KilnClientCrosshairApi), KILN_CLIENT_CROSSHAIR_VERSION } */
    /* The newest sample, copied into *out (out->size >= sizeof(KilnCrosshairHit)); *out_reason (may be NULL) says
     * whether it is fresh. KILN_OK with a reason; KILN_ERR_INVALID_ARGUMENT: out NULL or short; KILN_ERR_UNAVAILABLE:
     * the row client.crosshair is off (a dedicated server, safe idle, an unresolved build, the hook failed). */
    KilnResult(KILN_CALL* get)(KilnCrosshairHit* out, uint32_t* out_reason);
} KilnClientCrosshairApi;

#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
static_assert(sizeof(KilnCrosshairHit) == 88, "KilnCrosshairHit is 88 bytes");
static_assert(offsetof(KilnCrosshairHit, sequence) == 24 && offsetof(KilnCrosshairHit, position) == 48 &&
                  offsetof(KilnCrosshairHit, normal) == 72 && offsetof(KilnCrosshairHit, distance) == 84,
              "KilnCrosshairHit layout");
static_assert(sizeof(KilnClientCrosshairApi) == 16, "KilnClientCrosshairApi is 16 bytes");
#else
_Static_assert(sizeof(KilnCrosshairHit) == 88, "KilnCrosshairHit is 88 bytes");
_Static_assert(offsetof(KilnCrosshairHit, sequence) == 24 && offsetof(KilnCrosshairHit, position) == 48 &&
                   offsetof(KilnCrosshairHit, normal) == 72 && offsetof(KilnCrosshairHit, distance) == 84,
               "KilnCrosshairHit layout");
_Static_assert(sizeof(KilnClientCrosshairApi) == 16, "KilnClientCrosshairApi is 16 bytes");
#endif
#endif
