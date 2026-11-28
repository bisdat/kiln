/* SPDX-License-Identifier: MIT
 * kiln.camera v1 (since 0.2.17): camera overrides as leases. docs/kiln/services/camera.md.
 *
 * Three endpoints, each with its own lease domain:
 *  - SERVER_MODE: the server locks a player's stock camera mode for a time. It reaches vanilla clients. Server world.
 *  - LOCAL_VISUAL: a render-only camera for the local player of this client (a path, a follow profile, a lens, a
 *    shake). The player's input camera and aim are never moved. Client world.
 *  - REMOTE_VISUAL: the same visual request, sent by the server to a player whose client runs Kiln, through the
 *    transport bridge (bind_transport). Server world.
 *
 * A request is accepted (KILN_OK, a lease id at once) and decided later: `done` runs once on the owning world's next
 * Pre or a later one, never inside apply/update/end, and only while the owner lives. One lease wins per endpoint and
 * player; the same owner applying again is refused ALREADY_OWNED (use update), another owner wins only with a higher
 * priority, and then the old lease's ENDED(SUPERSEDED) comes before the new APPLIED. A successful change queues one
 * KilnCameraChange for every subscriber, after that operation's `done`, in operation order.
 *
 * Immediate returns never call `done`: KILN_ERR_INVALID_ARGUMENT (a malformed request, or owner 0 in apply, update or
 * end: a lease belongs to a real owner, one that can retire), KILN_ERR_WRONG_THREAD (not
 * the endpoint's world thread), KILN_ERR_UNAVAILABLE (a part's capability is off), KILN_ERR_NOT_FOUND (the endpoint's
 * world is not running), KILN_ERR_BUFFER_TOO_SMALL (KILN_CAMERA_MAX_PENDING operations wait already: ask again
 * later), KILN_ERR_REVOKED (the owner is gone). A refusal by the game or by the rules above arrives through `done`
 * with ok = 0 and a KILN_CAMERA_REASON_* code.
 *
 * Threads: apply/update/end on the endpoint's world thread (SERVER_MODE and REMOTE_VISUAL: the server tick thread;
 * LOCAL_VISUAL: the client world's thread). `done` and KilnCameraChanged run in that world's Pre, with one exception:
 * when the world stops, what is still open is finished with WORLD_STOPPED on the thread that destroys the world. A
 * callback therefore never touches game memory for a WORLD_STOPPED end or a STALE_WORLD refusal. No callback of this
 * table is ever called from the render thread. Strings in a result or change are borrowed for the callback.
 *
 * No game pointer crosses this table. The table grows only at its end; check header.size before a later slot.
 */
#ifndef KILN_CAMERA_H
#define KILN_CAMERA_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_CAMERA_SERVICE "kiln.camera"
#define KILN_CAMERA_VERSION 1u

typedef uint64_t KilnCameraLease; /* 0 = none. Never reused in a process */

/* KilnCameraRequest.endpoint */
enum { KILN_CAMERA_SERVER_MODE = 1, KILN_CAMERA_LOCAL_VISUAL = 2, KILN_CAMERA_REMOTE_VISUAL = 3 };
/* KilnCameraRequest.parts. MODE stands alone; PATH and PROFILE exclude each other; LENS and SHAKE combine freely. */
enum { KILN_CAMERA_MODE = 1, KILN_CAMERA_PATH = 2, KILN_CAMERA_PROFILE = 4, KILN_CAMERA_SHAKE = 8, KILN_CAMERA_LENS = 16 };
/* KilnCameraRequest.input_policy. MODE needs STOCK_MODE, a visual part VISUAL_ONLY; AIM_TOO is reserved and refused
 * (KILN_CAMERA_REASON_INPUT_POLICY). */
enum { KILN_CAMERA_VISUAL_ONLY = 0, KILN_CAMERA_STOCK_MODE = 1, KILN_CAMERA_AIM_TOO = 2 };
/* KilnCameraRequest.lens_source */
enum { KILN_CAMERA_LENS_NONE = 0, KILN_CAMERA_LENS_CONSTANT = 1, KILN_CAMERA_LENS_KEYS = 2 };
/* KilnCameraProfile.rotation_mode */
enum { KILN_CAMERA_FREE_ORBIT = 0, KILN_CAMERA_FOLLOW_HEADING = 1, KILN_CAMERA_FIXED_WORLD = 2 };
/* KilnCameraProfile.collision_policy. v1 has no camera collision: an authored view can clip geometry. */
enum { KILN_CAMERA_COLLISION_NONE = 0 };
/* KilnCameraRequest.transition_override (MODE) */
enum { KILN_CAMERA_TRANSITION_NONE = 0, KILN_CAMERA_TRANSITION_BLINK = 1, KILN_CAMERA_TRANSITION_LERP = 2 };
/* update(): timing */
enum { KILN_CAMERA_PRESERVE_TIMELINE = 0, KILN_CAMERA_RESTART_TIMELINE = 1 };
/* KilnCameraChange.kind */
enum { KILN_CAMERA_APPLIED = 1, KILN_CAMERA_UPDATED = 2, KILN_CAMERA_ENDED = 3 };
/* KilnCameraChange.end_reason */
enum {
    KILN_CAMERA_END_NONE = 0,
    KILN_CAMERA_END_RELEASED = 1,
    KILN_CAMERA_END_EXPIRED = 2,
    KILN_CAMERA_END_TARGET_LOST = 3,
    KILN_CAMERA_END_PLAYER_LEFT = 4,
    KILN_CAMERA_END_WORLD_STOPPED = 5,
    KILN_CAMERA_END_SUPERSEDED = 6,
    KILN_CAMERA_END_CAPABILITY_LOST = 7,
    KILN_CAMERA_END_OWNER_RETIRED = 8,
    KILN_CAMERA_END_PEER_DISCONNECTED = 9,
    KILN_CAMERA_END_AUTHORITY_TIMEOUT = 10,
    KILN_CAMERA_END_SAFETY_REJECTED = 11,
    KILN_CAMERA_END_TRANSPORT_TIMEOUT = 12,
    KILN_CAMERA_END_CANCELLED_BEFORE_APPLY = 13
};
/* KilnCameraResult.reason_code, KilnCameraChange.reason_code, get_visual's out_reason. Stable: decide by the code,
 * never by the text. */
enum {
    KILN_CAMERA_REASON_NONE = 0,
    KILN_CAMERA_REASON_BUSY = 1,
    KILN_CAMERA_REASON_ALREADY_OWNED = 2,
    KILN_CAMERA_REASON_UNKNOWN_LEASE = 3,
    KILN_CAMERA_REASON_FOREIGN_OWNER = 4,
    KILN_CAMERA_REASON_WRONG_ENDPOINT = 5,
    KILN_CAMERA_REASON_INPUT_POLICY = 6,
    KILN_CAMERA_REASON_REQUIRES_KILN_CLIENT = 7,
    KILN_CAMERA_REASON_PEER_UNAVAILABLE = 8,
    KILN_CAMERA_REASON_NEGOTIATION = 9,
    KILN_CAMERA_REASON_TARGET_UNAVAILABLE = 10,
    KILN_CAMERA_REASON_NO_CAMERA_FRAME = 11,
    KILN_CAMERA_REASON_STALE_WORLD = 12,
    KILN_CAMERA_REASON_EXPIRED_UPDATE = 13,
    KILN_CAMERA_REASON_COLLISION = 14,
    KILN_CAMERA_REASON_TRANSPORT_TIMEOUT = 15,
    KILN_CAMERA_REASON_MALFORMED_REMOTE = 16,
    KILN_CAMERA_REASON_RESOURCE_BUSY = 17,
    KILN_CAMERA_REASON_FRAME_FAULT = 18,
    KILN_CAMERA_REASON_SNAPSHOT_BUSY = 19,
    KILN_CAMERA_REASON_SNAPSHOT_STALE = 20,
    KILN_CAMERA_REASON_BOUNDS = 21,
    KILN_CAMERA_REASON_RATE_LIMIT = 22,
    KILN_CAMERA_REASON_LEASE_KIND = 23,
    KILN_CAMERA_REASON_CANCELLED = 24,
    KILN_CAMERA_REASON_CLIENT_CAPABILITY = 25,
    KILN_CAMERA_REASON_INSUFFICIENT_LIFETIME = 26,
    KILN_CAMERA_REASON_BODY_TOO_LARGE = 27
};
/* KilnCameraResult.commit_scope: what an ok result proves. SERVER_STATE: the server's state holds the value (a
 * vanilla client sends no acknowledgement). CLIENT_FRAME: a frame of that client was drawn with it. */
enum { KILN_CAMERA_COMMIT_NONE = 0, KILN_CAMERA_COMMIT_SERVER_STATE = 1, KILN_CAMERA_COMMIT_CLIENT_FRAME = 2 };
/* KilnCameraResult.flags. MAY_HAVE_APPLIED: not confirmed, but a frame may have shown it (end the lease).
 * ALREADY_ENDED: end() of a lease of yours that had ended; no second ENDED follows. */
enum { KILN_CAMERA_MAY_HAVE_APPLIED = 1, KILN_CAMERA_ALREADY_ENDED = 2 };

/* capabilities(): one bit per row camera.<name> (docs/kiln/services/caps.md). */
#define KILN_CAMERA_CAP_SERVER_MODE       (UINT64_C(1) << 0)
#define KILN_CAMERA_CAP_LOCAL_POSE        (UINT64_C(1) << 1)
#define KILN_CAMERA_CAP_LOCAL_FOV         (UINT64_C(1) << 2)
#define KILN_CAMERA_CAP_LOCAL_PROFILE     (UINT64_C(1) << 3)
#define KILN_CAMERA_CAP_LOCAL_SHAKE       (UINT64_C(1) << 4)
#define KILN_CAMERA_CAP_RENDERED_SNAPSHOT (UINT64_C(1) << 5)
#define KILN_CAMERA_CAP_REMOTE            (UINT64_C(1) << 6)
#define KILN_CAMERA_CAP_RESOURCE_PROFILE  (UINT64_C(1) << 7) /* reserved: never set by v1 */

#define KILN_CAMERA_MAX_KEYS 256u
#define KILN_CAMERA_MAX_DURATION_MS 120000u
#define KILN_CAMERA_MAX_PENDING 64u

/* Position in world metres, rotation as a unit quaternion (x, y, z, w); +Z is forward, +Y up. */
typedef struct KilnCameraPose {
    double position[3];
    float rotation[4];
} KilnCameraPose;

typedef struct KilnCameraKey {
    uint32_t at_ms, reserved;
    KilnCameraPose pose;
    float fov_y_degrees; /* LENS_KEYS only; otherwise exactly 0 */
    uint32_t reserved2;
} KilnCameraKey;

typedef struct KilnCameraProfile {
    uint32_t size, follow_entity, rotation_mode, collision_policy;
    /* follow_entity 0: the local player. distance: metres from the pivot, absolute (not added to the game's own
     * distance, not scaled with the target). offset_local: metres in the target's axes; offset_world_y: metres up. */
    float distance, offset_local[3], offset_world_y, rotation[4];
    /* rotation. FREE_ORBIT: identity (the view keeps the game's orbit). FOLLOW_HEADING: an offset on the target's
     * upright heading (yaw only). FIXED_WORLD: the world orientation. */
} KilnCameraProfile;

typedef struct KilnCameraShake {
    float amplitude_metres, frequency_hz;
    uint32_t falloff_ms, seed; /* translation only, in the camera's axes; fades over the last falloff_ms */
} KilnCameraShake;

typedef struct KilnCameraRequest {
    uint32_t size, parts, endpoint, player_entity; /* player 0: LOCAL_VISUAL's local player; else required */
    uint32_t input_policy, duration_ms, blend_in_ms, blend_out_ms; /* duration 1..KILN_CAMERA_MAX_DURATION_MS */
    int32_t priority; /* -1000..1000 */
    uint32_t lens_source;
    float fov_y_degrees; /* LENS_CONSTANT: 1..179; otherwise 0 */
    uint8_t camera_id, transition_override, reserved[2]; /* MODE: stock camera id 0..45, KILN_CAMERA_TRANSITION_* */
    KilnCameraProfile profile;
    KilnCameraShake shake;
    uint32_t key_count;
    const KilnCameraKey* keys; /* PATH: 1..KILN_CAMERA_MAX_KEYS, copied before the call returns; otherwise NULL, 0 */
} KilnCameraRequest;

typedef struct KilnCameraResult {
    uint32_t size, ok, reason_code, commit_scope, flags, endpoint, player_entity, local_generation;
    KilnCameraLease lease;
    uint64_t revision, frame;
    KilnStr reason; /* words for a log; borrowed for the callback */
} KilnCameraResult;

typedef struct KilnCameraChange {
    uint32_t size, kind, endpoint, player_entity, local_generation, end_reason, reason_code, reserved;
    KilnCameraLease lease;
    uint64_t revision, frame;
    KilnStr reason; /* borrowed for the callback */
} KilnCameraChange;

/* The camera a frame was drawn with (get_visual). Values only. */
typedef struct KilnCameraVisual {
    uint32_t size, valid, local_generation, reserved;
    uint64_t frame, target_sample_step, captured_monotonic_ns;
    KilnCameraPose pose;
    float fov_y_radians, aspect, near_plane, far_plane;
} KilnCameraVisual;

typedef void(KILN_CALL* KilnCameraDone)(const KilnCameraResult* result, void* user);
typedef void(KILN_CALL* KilnCameraChanged)(const KilnCameraChange* change, void* user);

/* The transport bridge of REMOTE_VISUAL (framework-private: Kiln.Camera binds it; a mod does not).
 * submit: op 1 apply, 2 update keeping the timeline, 3 update restarting it, 4 end (request NULL). KILN_OK means
 * accepted, not applied: `done` follows on a server Pre, never inside submit. The bridge copies the request before it
 * returns. caps: the part bits the player's client offered; any thread. retire: the author is gone; server tick. */
typedef struct KilnCameraTransport {
    KilnServiceHeader header;
    KilnResult(KILN_CALL* submit)(KilnOwner author, uint32_t op, KilnCameraLease lease, uint64_t revision,
                                  const KilnCameraRequest* request, KilnCameraDone done, void* user);
    KilnResult(KILN_CALL* caps)(KilnEntity player, uint64_t* mask);
    void(KILN_CALL* retire)(KilnOwner author);
} KilnCameraTransport;

typedef struct KilnCameraApi {
    KilnServiceHeader header; /* { sizeof(KilnCameraApi), KILN_CAMERA_VERSION } */

    /* Which KILN_CAMERA_CAP_* work for `endpoint` (KILN_CAMERA_SERVER_MODE, ...). A bit says the capability exists,
     * not that a player or a frame is there now. REMOTE_VISUAL with a player: the bits that player's client offered.
     * Any thread. KILN_ERR_INVALID_ARGUMENT: no such endpoint, or mask is NULL. */
    KilnResult(KILN_CALL* capabilities)(uint32_t endpoint, KilnEntity player, uint64_t* mask);

    /* A new lease. See the head of this file for the answer and the threads. */
    KilnResult(KILN_CALL* apply)(KilnOwner owner, const KilnCameraRequest* request, KilnCameraLease* out_lease,
                                 KilnCameraDone done, void* user);

    /* Replaces the lease's whole request (not a merge). The endpoint, the player and the kind (MODE or visual) stay.
     * timing: KILN_CAMERA_PRESERVE_TIMELINE or KILN_CAMERA_RESTART_TIMELINE. A refused update keeps the old revision. */
    KilnResult(KILN_CALL* update)(KilnOwner owner, KilnCameraLease lease, uint32_t timing,
                                  const KilnCameraRequest* request, KilnCameraDone done, void* user);

    /* Ends a lease of `owner`. Works while a capability is off. Work of the lease that still waits is cancelled first
     * (its `done`: KILN_CAMERA_REASON_CANCELLED). `done` and ENDED come when the release has happened. */
    KilnResult(KILN_CALL* end)(KilnOwner owner, KilnCameraLease lease, KilnCameraDone done, void* user);

    /* Every change of every lease. Any thread. unsubscribe stops future calls and does not wait for a running one. */
    KilnResult(KILN_CALL* subscribe)(KilnOwner owner, KilnCameraChanged fn, void* user, KilnHandle* out);
    KilnResult(KILN_CALL* unsubscribe)(KilnHandle handle);

    /* The camera of the newest frame. Any thread; never waits. KILN_OK with out->valid 0 and *out_reason
     * NO_CAMERA_FRAME, SNAPSHOT_BUSY or SNAPSHOT_STALE when there is none to give. KILN_ERR_UNAVAILABLE: the row
     * camera.rendered_snapshot is off. */
    KilnResult(KILN_CALL* get_visual)(KilnCameraVisual* out, uint32_t* out_reason);

    /* Framework-private: one bridge at a time; the table is copied. unbind after the bridge drained its work. */
    KilnResult(KILN_CALL* bind_transport)(KilnOwner bridge_owner, const KilnCameraTransport* table);
    KilnResult(KILN_CALL* unbind_transport)(KilnOwner bridge_owner);
    /* The client ended a remote lease by itself. Checked against the bridge, the lease and its revision. */
    KilnResult(KILN_CALL* transport_ended)(KilnOwner bridge_owner, KilnCameraLease lease, uint64_t revision,
                                           uint32_t end_reason, uint32_t reason_code, KilnStr reason);
} KilnCameraApi;

#ifdef __cplusplus
}
#endif
#endif /* KILN_CAMERA_H */
