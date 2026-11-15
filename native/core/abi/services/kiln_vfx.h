/* SPDX-License-Identifier: MIT
 * kiln.vfx v1: play the game's VFX from code and keep an unknown VFX GUID from killing the client (docs/kiln/services/vfx.md,
 * research docs/re/vfx.md). Client only: a dedicated server has no VFX, and the service
 * is absent there. Kiln.Vfx relays server calls to Kiln clients.
 *
 * The guard is the point. The game aborts on a VFX GUID it did not register at init. Kiln detours VfxCreateInstance and the
 * FxController's start: an unknown GUID gets handle 0 (the path an empty instance pool already takes) and one log line.
 *
 * Threads
 *  - Every function may be called from any thread. Requests are queued and run inside the game's FxController update
 *    (client_update_fx_controller), the code that makes the game's own VFX calls. A handle is returned at once in state
 *    PENDING. No world means no FxController, so a request waits there for 10 s and then fails (TIMEOUT).
 *  - state / info / status read Kiln's own bookkeeping, never game memory. is_registered reads the VFX system's map.
 * Handles
 *  - KilnVfx ids are Kiln's (1..2^31-1, never reused within a process). A finished handle reads STOPPED or FAILED for at least
 *    60 s, then NONE.
 * Fail soft
 *  - Signatures missing on this build (features vfx.guard, vfx.play): the service is not registered, or play returns 0. A
 *    fault in a game call turns kiln.vfx off for the session with an error line.
 * Owners
 *  - The service cannot see owner_revoke: call stop_owner when an owner goes away (Kiln.Vfx does for mods).
 * The resource budget (since 0.2.0-dev.29, members after bind_guid; chapter 17.1 §5, I1, I5)
 *  - The game's VFX system holds a fixed number of VFX resources (its count limit: 2,048 on client 26430625, of which the
 *    game's own 1,575 leave 473 for mods). resource_budget reads it, define_vfx_result defines a VFX with an answer
 *    (registered in this process, accepted for the next start, or refused with a code) and vfx_registration reads that
 *    answer again by its registration id. Shapes: kiln_capacity.h. Nothing of it changes the game: the count limit is
 *    the game's own (a larger table is chapter 17.1 §4, not this). */
#ifndef KILN_VFX_H
#define KILN_VFX_H
#include "../kiln_api.h"
#include "kiln_capacity.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_VFX_SERVICE "kiln.vfx"
#define KILN_VFX_VERSION 1u

typedef uint32_t KilnVfx; /* 0 = none */

/* KilnVfxPlay.flags */
enum {
    KILN_VFX_AT = 1u,            /* play at `position`, `rotation`, `scale` */
    KILN_VFX_ON = 2u,            /* attach to client-world `entity` at `slot` with the offset; moves with it, stops when it
                                    is gone */
    KILN_VFX_VERIFY = 4u,        /* with ON and AT: `position` is where the entity should be (the server's view). Attach only
                                    if the client's entity of that id is within verify_radius; else play at `position` */
    KILN_VFX_TEST_FX = 8u,       /* test only, unknown GUIDs only: skip Kiln's check and go through the FxController, so the
                                    guard in the game's own path must catch it */
    KILN_VFX_TEST_CREATE = 16u,  /* test only, unknown GUIDs only: skip Kiln's check and call the game's RenderCreateVfx
                                    directly, so the guard on VfxCreateInstance must catch it */
    KILN_VFX_WAIT_GUID = 32u,    /* `vfx` is not known yet: the entry waits, up to the 10 s every request has, for bind_guid.
                                    For names only another thread can resolve */
};

/* AttachmentSlot values: keen::AttachmentSlot (Root 0, HandR 8, HandL 9, Generic00 16, ..., Generic19 35). */
#define KILN_VFX_SLOTS 36u

typedef struct KilnVfxPlay {
    uint32_t size;            /* sizeof(KilnVfxPlay) */
    uint32_t flags;           /* KILN_VFX_* */
    uint8_t vfx[16];          /* VfxResource GUID, game byte order (.NET Guid.ToByteArray()) */
    KilnVec3 position;        /* AT: metres */
    float rotation[4];        /* AT: x, y, z, w. All zero = identity */
    float scale;              /* AT: 0 = 1 */
    KilnEntity entity;        /* ON */
    uint32_t slot;            /* ON: AttachmentSlot, 0 = the entity's root */
    float local_offset[3];    /* ON: keen::ecs::Offset.localOffset, metres in the slot's space */
    float world_offset[3];    /* ON: keen::ecs::Offset.worldOffset, metres in world axes */
    float offset_rotation[4]; /* ON: keen::ecs::Offset.orientationOffset, all zero = identity */
    float verify_radius;      /* VERIFY: metres, 0 = 10 */
    int64_t lifetime_ns;      /* 0 = one-shot: the game has no "finished" signal, so Kiln stops it after the length of the effect. A
                                 mod's block: its delay, duration and longest particle life. Otherwise what its resource gives:
                                 the larger of killTimeInSeconds and its duration + lifetime parameters, 10 s if it says
                                 nothing, 1 to 60 s. > 0 = stopped after that time. < 0 = until stop (a loop; ON also
                                 ends when the entity goes) */
} KilnVfxPlay;

/* state() */
enum {
    KILN_VFX_NONE = 0,    /* unknown or expired handle */
    KILN_VFX_PENDING = 1, /* queued: waiting for the next FxController update */
    KILN_VFX_PLAYING = 2, /* the FxController holds it. The game may still be loading its pipelines: first plays start late */
    KILN_VFX_STOPPED = 3, /* ended (see KilnVfxInfo.reason) */
    KILN_VFX_FAILED = 4,  /* never started (see KilnVfxInfo.reason) */
};

/* KilnVfxInfo.reason */
enum {
    KILN_VFX_END_NONE = 0,
    KILN_VFX_END_FINISHED = 1,    /* the FxController dropped it: the lifetime ran out and the instance went */
    KILN_VFX_END_STOPPED = 2,     /* stop / stop_owner */
    KILN_VFX_END_ENTITY_GONE = 3, /* ON: the entity is not in the client world */
    KILN_VFX_END_UNKNOWN_VFX = 4, /* the game has no VFX with that GUID: ignored */
    KILN_VFX_END_START_FAILED = 5,/* the FxController returned no handle (its pool is full) */
    KILN_VFX_END_TIMEOUT = 6,     /* waited 10 s: no FxController update (no client world) or the VFX system not ready */
    KILN_VFX_END_WORLD_ENDED = 7, /* the client world ended */
    KILN_VFX_END_FAULT = 8,       /* a game call faulted: kiln.vfx is off for this session */
    KILN_VFX_END_REFUSED = 9,     /* a test flag on a GUID the game knows */
    KILN_VFX_END_NO_INSTANCE = 10,/* no instance after 5 s: the body has no such slot, or the instance pool is full */
};

/* KilnVfxInfo.flags */
enum {
    KILN_VFX_INFO_ATTACHED = 1u,  /* ON took effect */
    KILN_VFX_INFO_UNMATCHED = 2u, /* VERIFY failed: played at `position` without attaching */
};

typedef struct KilnVfxInfo {
    uint32_t size;       /* set by the caller: sizeof(KilnVfxInfo) */
    uint32_t state;      /* KILN_VFX_* */
    uint32_t reason;     /* KILN_VFX_END_* */
    uint32_t flags;      /* KILN_VFX_INFO_* */
    uint32_t fx_handle;  /* the FxController's handle, 0 = none */
    uint32_t instance;   /* the VFX instance behind it, 0 = none yet (an attachment that is not resolved) */
    uint32_t thread;     /* thread that made the game call */
    KilnEntity entity;
    uint64_t queued_ms, started_ms, ended_ms; /* GetTickCount64 style */
    KilnVec3 position;
    int64_t lifetime_ns; /* what the play settled on: < 0 until stop; 0 before it started */
} KilnVfxInfo;

/* KilnVfxStatus.flags */
enum {
    KILN_VFX_HAS_GUARD = 1u,  /* the guard is hooked and its map layout checked out */
    KILN_VFX_HAS_PLAY = 2u,   /* play can reach the game (signatures resolved, no fault) */
    KILN_VFX_CONTROLLER = 4u, /* an FxController update ran and no world ended since */
    KILN_VFX_SYSTEM = 8u,     /* the VFX system's map holds entries */
    KILN_VFX_SIM = 16u,       /* simulated game (kiln/config/vfx.json "sim", labs and tests only) */
};

typedef struct KilnVfxStatus {
    uint32_t size;          /* set by the caller: sizeof(KilnVfxStatus) */
    uint32_t flags;         /* KILN_VFX_HAS_* ... */
    uint32_t registered;    /* VFX in the game's map */
    uint32_t map_capacity;
    uint32_t thread;        /* thread of the FxController update, 0 = not seen */
    uint32_t live;          /* Kiln's handles pending or playing */
    uint64_t played;        /* Kiln's plays that reached the FxController */
    uint64_t failed;        /* Kiln's plays that never started */
    uint64_t refused;       /* the guard refused: requests that would have aborted the game */
    uint64_t updates;       /* FxController updates seen */
    uint64_t controller;    /* the FxController's address, 0 = none */
} KilnVfxStatus;

typedef struct KilnVfxApi {
    KilnServiceHeader header;
    KilnResult(KILN_CALL* status)(KilnVfxStatus* out);
    KilnVfx(KILN_CALL* play)(KilnOwner owner, const KilnVfxPlay* play); /* 0: unavailable, bad arguments or too many live */
    /* immediate 0: the game's stop. With killTimeInSeconds 0 the instance goes at once; otherwise it stays that many seconds at
       most, then goes whatever is alive (10 for almost every VFX). 1: the instance is destroyed at the next update. */
    KilnResult(KILN_CALL* stop)(KilnVfx vfx, int immediate);
    /* A named parameter of the effect: name_hash = FNV-1a of the name, type_id = keen::DsTypeId, at most 16 B. Set before the start
       (KILN_VFX_WAIT_GUID holds a play until bind_guid), a value the VFX describes goes into the block it starts with; anything else
       is applied with the game's setter once the instance exists. A name or type the VFX does not have is ignored by the game. */
    KilnResult(KILN_CALL* set_param)(KilnVfx vfx, uint32_t name_hash, uint32_t type_id, const void* data, size_t size);
    uint32_t(KILN_CALL* state)(KilnVfx vfx);
    KilnResult(KILN_CALL* info)(KilnVfx vfx, KilnVfxInfo* out);
    uint32_t(KILN_CALL* stop_owner)(KilnOwner owner, int immediate);
    /* 1 registered, 0 not, -1 the VFX system is not known or not ready yet */
    int32_t(KILN_CALL* is_registered)(const uint8_t guid[16]);
    /* Holds a reference on the VFX, for the session, so its first real play does not start late. Queued like play. */
    KilnResult(KILN_CALL* preload)(const uint8_t guid[16]);
    KilnResult(KILN_CALL* status_json)(char* buf, size_t cap, size_t* out_len); /* status + recent handles */
    /* A VFX defined from code: {"format": 1, "mod": id, "dir": folder, "vfx": {...}}, the same blocks as a JSON file in a mod's blocks folder.
       Validates and builds it. The report is JSON (docs/kiln/services/vfx-cook.md): each block with its GUID, timing and status "registered", or "pending"
       when it is there from the next start, since the game enumerates VFX once. */
    KilnResult(KILN_CALL* define_json)(KilnOwner owner, KilnStr json, char* report, size_t cap, size_t* out_len);
    KilnResult(KILN_CALL* build_status)(char* buf, size_t cap, size_t* out_len); /* JSON: what was built, the budget, the fingerprint */
    /* The GUID of a play made with KILN_VFX_WAIT_GUID. All zero: no such VFX, the entry fails as unknown. */
    KilnResult(KILN_CALL* bind_guid)(KilnVfx vfx, const uint8_t guid[16]);

    /* ---- the resource budget: appended since 0.2.0-dev.29 (version stays 1). Check header.size covers a member ---- */
    /* The VFX resources of the game's VFX system (process scope; units: resource records). capacity = the count limit the
     * game enforces, read from its VFX system (sys + caps/vfx.map + caps/vfx.map_limit_relative); game_used = the game's
     * own VfxResources (counted in its archive); kiln_used = Kiln's VFX of loaded mods that are in the game's table with
     * the bytes Kiln served; reserved = the rest of the table: Kiln's VFX of a mod that is no longer loaded (they stay for
     * the process), and resources of other loaders (the table's count less the game's and Kiln's). readiness NOT_READY
     * until kiln.vfx read the table at an FxController update of a client world with the system's state ready (2);
     * ACCOUNTING_MISMATCH when the counts do not add up; UNKNOWN_EXTERNAL when the game's own count could not be read
     * from its archive. A snapshot the update thread keeps: never a read of game memory. Any thread.
     * KILN_ERR_INVALID_ARGUMENT: out NULL or out->size too small. KILN_ERR_UNAVAILABLE: this build cannot read the count
     * limit (capability vfx.budget). */
    KilnResult(KILN_CALL* resource_budget)(KilnCapacityInfo* out);
    /* Defines one VFX block of a mod with an answer. name: the block's name (letters, digits, '-', '_', '.'; at most
     * 128). typed_vfx_json: {"format": 1, "mod": "<mod id>", "dir": "<the mod's folder>", "block": {<one VfxBlock, as in
     * a blocks file's "vfx" section>}}; the same validation and builder as a blocks file (docs/kiln/services/vfx-cook.md).
     * The game lists its VFX once, before any mod loads, so a definition registers in this process only when the block
     * is one the start built with this same content:
     *  - REGISTERED (slots_consumed 1): the system's state is 2, its GUID map has the GUID (a read of the map, never
     *    vfx/Resolve, which would acquire it) and the table's resource holds the bytes Kiln served.
     *  - timing KILN_DEFINE_REQUIRE_REGISTERED and not registered: refused, nothing written. REGISTRY_CLOSED (a new GUID,
     *    or a content the start did not build), NOT_READY (built at this start, the table not read yet: the first
     *    FxController update of a client world reads it), GAME_REFUSED (not in the game's table, or other bytes there),
     *    OWNER_INACTIVE, ID_COLLISION (the GUID is another mod's built VFX), CAPACITY_EXCEEDED.
     *  - A call refused for its owner, a collision, the capacity or storage answers that refusal; a VFX the definition still
     *    occupies in the game's table is RETAINED with its slot (never reported free).
     *  - timing KILN_DEFINE_ALLOW_NEXT_START: when only the moment stands in the way, the block is written (atomically)
     *    to <kiln>/data/vfx/pending/<mod>/<name>.json with its registration id, and the next start builds it before the
     *    game lists its VFX (ACCEPTED_NEXT_START, slots_consumed 0; STORAGE_FAILURE when it cannot be written). A new GUID
     *    that would not fit (the game's, Kiln's and other resources now, the new ones accepted in this process, and this
     *    one, over the capacity) is CAPACITY_EXCEEDED, never accepted. The next start checks every stored block again.
     * The same definition again answers the same and writes nothing new. report: {"reason": null | "...", "warnings":
     * [...]}, at most 4096 bytes: with less room KILN_ERR_BUFFER_TOO_SMALL before anything is read (*out_len 4096).
     * KILN_ERR_INVALID_ARGUMENT: not such a document, a bad name, a block the builder refuses (the report says why), a bad
     * timing, out NULL or too small; nothing was changed. KILN_ERR_UNAVAILABLE: capability blocks.vfx_registration is
     * off. Any thread (Kiln.Blocks calls it on the tick thread); never a game call. */
    KilnResult(KILN_CALL* define_vfx_result)(KilnOwner owner, KilnStr name, KilnStr typed_vfx_json, uint32_t timing,
                                             KilnDefinitionReply* out, char* report, size_t capacity, size_t* out_len);
    /* The answer for registration_id now: what became of the definition since its call (registered once the table was
     * read, kept for a mod that is no longer loaded: RETAINED with OWNER_INACTIVE, superseded by a later definition of
     * the block, stored for the next start). A definition stored at an earlier start is known again at this one: its
     * receipt says what this start made of it. ok = 0 with NOT_FOUND for an id this process does not know. owner may
     * be 0. report and its room as define_vfx_result. Any thread. KILN_ERR_INVALID_ARGUMENT: registration_id NULL or all
     * zero, out NULL or too small. KILN_ERR_UNAVAILABLE: capability blocks.vfx_registration is off. */
    KilnResult(KILN_CALL* vfx_registration)(KilnOwner owner, const uint8_t registration_id[16], KilnDefinitionReply* out,
                                            char* report, size_t capacity, size_t* out_len);
} KilnVfxApi;

#ifdef __cplusplus
}
#endif
#endif
