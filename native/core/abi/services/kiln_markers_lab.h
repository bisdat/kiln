/* SPDX-License-Identifier: MIT
 * kiln.markers.lab v1 (kiln-markers.dll): LAB SERVERS ONLY. Published only when the file kiln/control/markers.lab exists
 * when a dedicated server starts; absent everywhere else. It lets a lab test mod (mods/tests/markers-test, through
 * Kiln.Markers' MarkersLab) prove the anchors of kiln.markers (chapter 04 §4.2) on a dedicated server: the follow read of a
 * lease now, the counters of the create/destroy observers (and on how many threads they ran), and an entity created with
 * an explicit id and nothing but a position (no SpawnTime), as the research probe did. Not part of the mod API; may
 * change in any release. */
#ifndef KILN_MARKERS_LAB_H
#define KILN_MARKERS_LAB_H
#include "../kiln_api.h"
#include "kiln_markers.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_MARKERS_LAB_VERSION 1u
#define KILN_MARKERS_LAB_SERVICE "kiln.markers.lab"

typedef struct KilnMarkersLabFollow {
    uint32_t size;           /* set by the caller */
    uint32_t reason;         /* KILN_MARKER_READY, _FOLLOW_MISSING, _BAD_POSITION or _ANCHOR_ENDED, read now */
    uint64_t incarnation;    /* the lease's incarnation (the watch serial) */
    uint64_t world_epoch;
    uint32_t world_kind, entity;
    KilnVec3 position;       /* READY: where it is now; else the last known position */
} KilnMarkersLabFollow;

typedef struct KilnMarkersLabStats {
    uint32_t size;            /* set by the caller */
    uint32_t threads;         /* distinct threads the create/destroy/network-create observers ran on (16 told apart) */
    uint32_t other_threads;   /* ... of them none of the world threads seen at a Post (the game's workers) */
    uint32_t watched;         /* incarnations watched now */
    uint32_t leases, references;
    uint64_t creates, destroys, network_creates; /* observer calls since start */
    uint64_t ended;           /* incarnations ended by a structural attempt, a world stop or a guard */
    uint64_t serials;         /* incarnations minted since start */
} KilnMarkersLabStats;

typedef struct KilnMarkersLabApi {
    KilnServiceHeader header; /* { sizeof(KilnMarkersLabApi), KILN_MARKERS_LAB_VERSION } */
    /* The follow read of the owner's lease now, as a world's Post does it for a stored record (the lease's world thread,
     * between steps): KILN_OK; KILN_ERR_NOT_FOUND for an unknown or foreign lease; KILN_ERR_WRONG_THREAD. */
    KilnResult(KILN_CALL* follow_now)(KilnOwner owner, KilnMarkerAnchor anchor, KilnMarkersLabFollow* out);
    /* Counters (any thread). */
    KilnResult(KILN_CALL* stats)(KilnMarkersLabStats* out);
    /* The game's immediate create on the server world with requested_id (0: the engine picks one), the template and only a
     * position (no SpawnTime, no other component): the server tick thread, Pre or Post. KILN_OK with *out the created id;
     * KILN_ERR_NOT_FOUND when the engine refused; KILN_ERR_INTERNAL when it faulted. */
    KilnResult(KILN_CALL* create_raw)(uint32_t requested_id, const uint8_t template_guid[16], KilnVec3 position, KilnEntity* out);
} KilnMarkersLabApi;

#if UINTPTR_MAX == 0xffffffffffffffffu
#ifdef __cplusplus
static_assert(sizeof(KilnMarkersLabFollow) == 56, "KilnMarkersLabFollow is 56 bytes in v1");
static_assert(sizeof(KilnMarkersLabStats) == 64, "KilnMarkersLabStats is 64 bytes in v1");
static_assert(sizeof(KilnMarkersLabApi) == 32, "KilnMarkersLabApi is 32 bytes in v1");
#endif
#endif

#ifdef __cplusplus
}
#endif
#endif /* KILN_MARKERS_LAB_H */
