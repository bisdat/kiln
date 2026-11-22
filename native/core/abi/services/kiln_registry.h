/* SPDX-License-Identifier: MIT
 * kiln.registry v1 (since 0.2.0-dev.31): Kiln's registries. The game's data as Kiln ids and Kiln fields (items, recipes,
 * loot, drops and balance in the first release), changed by mods in one data stage per start and reported in one place
 * (<game>/kiln/state/registry.json, schema/state-registry.schema.json). Mods name only Kiln ids ("game:<stable name>",
 * "<mod id>:<name>") and Kiln fields ("maxStack", "damage.fire"); Kiln maps them onto this game build and refuses, per
 * edit, what it cannot map there. This table's layout and the meaning of its members are kept from now on (append-only,
 * as every table: the live-change members of registry P2 will be appended at its tail, check header.size); the registry
 * schemas themselves (registries, fields, ids) are experimental until API 1.0.0 (C#: KILNX031) and change only with an id
 * migration.
 *
 * Mods: Kiln binds each owner to one mod id, the one it was made for (a Kiln mod's owner from Kiln.Host: its manifest id; a
 * native mod's: its native.json id; a plugin's: the id it was loaded under), the owner's id without its kind prefix, folded
 * to lower case (as a:CRATE selects a:crate). A batch runs at that mod's place in the order; a batch that names another mod
 * (compared in lower case too) is refused.
 *
 * Capability rows: kiln.registry (the service), registry.<registry> and registry.<registry>.<field> (per game build, from
 * Kiln's verified mapping of that build). Side: both (a dedicated server and a game client each run their own stage).
 *
 * The stage (once per start, on the stage thread: the game's main thread, the thread kiln_boot and Kiln.Host's Init ran
 * on, at kiln.assets' resources-ready, before the game reads any resource Kiln changes):
 *  - every mod's add, then every mod's alter, then every mod's final (KILN_REGISTRY_PHASE_ADD/ALTER/FINAL);
 *  - mods in the runtime load order (the launcher profile's order, from Kiln.Host; without .NET, kiln.content's mod
 *    folders). A batch of a mod that order does not name (a plugin's, a native mod's) runs after every mod it names, by
 *    mod id (byte order);
 *  - one mod's turn in a stage: its data files' edits (content/<file>.json "registry", files by name), then the batches it
 *    submitted before the stage, in the order submitted, then its stage callback (subscribe_stage);
 *  - later in load order wins and is reported; relative changes stack (x4 then x2 = x8); "exclusive" keeps the strict
 *    rule (the later conflicting edit is refused whole); a mod's final sees add + alter and its own final edits, never
 *    another mod's final; Kiln's older paths (boot resource patches, data packs) and the archive writer's values lie
 *    below every registry edit and are never refused because of one.
 * When the stage ends the phase is START: restart fields are final (baked into the resources the game reads). Live fields
 * are applied at each world load, above data packs and below code changes (kiln.content), and the phase is WORLDLOAD.
 * v1: the phase stays WORLDLOAD after that world stops (status names the last application of the live fields).
 *
 * Edits: the edit IR, one JSON document per batch (schema/registry-ir.schema.json): {"ir": 1, "mod": "<mod id>",
 * "stage": "add" | "alter" | "final", "source": "<what reports name>", "edits": [...]}. Field edits, list edits, entry
 * add / remove, and the Advanced tier's raw member paths ("raw", checked per entry on this game build). A malformed edit
 * is refused with its reason, the rest of the batch stands.
 *
 * Conventions for every member:
 *  - Strings in are UTF-8 KilnStr, not NUL-terminated; ptr may be NULL only when len is 0. Kiln copies what it keeps and
 *    never holds the caller's pointer after the call returns. An IR document or a selector is at most
 *    KILN_REGISTRY_MAX_IR_BYTES and nests at most 32 levels; a registry, id, field or mod name at most 1024 bytes (longer:
 *    KILN_ERR_INVALID_ARGUMENT).
 *  - Text out of a reading member (schema_json, read, select, report_json): copied into buf, NUL-terminated, cut at cap
 *    (at most cap - 1 bytes of text); *out_len (out_len may be NULL) = the whole text's length in bytes, without the NUL.
 *    KILN_ERR_BUFFER_TOO_SMALL when it did not fit: call again with cap >= *out_len + 1. buf may be NULL only when cap is
 *    0 (a size query). The text can grow between two calls (a later phase): loop on BUFFER_TOO_SMALL.
 *  - A report out of a member that changes something (submit): optional (NULL with cap 0 for none), the
 *    same copy rule, but its result is about the change, never KILN_ERR_BUFFER_TOO_SMALL (as kiln.content's apply_json).
 *  - Records: size is the caller's sizeof on the way in. A size below v1's is refused (KILN_ERR_INVALID_ARGUMENT); a
 *    larger one is accepted, Kiln fills the members it knows and sets size to the bytes it filled (a newer caller on an
 *    older Kiln sees which members are v1's). A record Kiln passes to a callback has size set to what Kiln filled.
 *  - Owners: owner 0 (the core) is refused (KILN_ERR_INVALID_ARGUMENT); submit and subscribe_stage refuse a revoked owner
 *    with KILN_ERR_REVOKED, and an owner Kiln cannot bind to a mod with KILN_ERR_INVALID_ARGUMENT. v1 does not take a
 *    revoked owner's queued batches or subscriptions away: unsubscribe before the owner goes; edits the stage already took
 *    stay until the next start.
 *  - Callbacks: the record pointer is valid only during the call. fn and user stay valid until unsubscribe returned. A
 *    native fault inside a stage callback is caught (kiln.log), the subscription is removed, and that turn's edits are
 *    dropped and reported; a fault inside a ready callback is caught and logged (v1 keeps that subscription).
 *  - Results: KILN_OK; KILN_ERR_INVALID_ARGUMENT (a NULL required pointer, a bad size, owner 0, a malformed document,
 *    an unknown registry, field or view); KILN_ERR_NOT_FOUND (no such entry or handle); KILN_ERR_UNAVAILABLE (not now:
 *    the phase does not allow it, the field is not mapped on this game build); KILN_ERR_WRONG_THREAD;
 *    KILN_ERR_BUFFER_TOO_SMALL (reading members only); KILN_ERR_REVOKED.
 */
#ifndef KILN_REGISTRY_H
#define KILN_REGISTRY_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_REGISTRY_SERVICE "kiln.registry"
#define KILN_REGISTRY_VERSION 1u

#define KILN_REGISTRY_MAX_IR_BYTES (16u * 1024u * 1024u)

/* When a field's change takes effect (the schema's "applies"; C# RegistryApplies has the same numbers). Permanent tags. */
enum {
    KILN_REGISTRY_APPLIES_NOW = 1,       /* read by the game each time it is used: a change acts at once */
    KILN_REGISTRY_APPLIES_WORLDLOAD = 2, /* read when a world loads: a change acts at the next world load */
    KILN_REGISTRY_APPLIES_RESTART = 3    /* read when the game starts: a change acts at the next start */
};

/* KilnRegistryStatus.phase, KilnRegistryReady.phase, KilnRegistryStageInfo.stage. Permanent tags. */
enum {
    KILN_REGISTRY_PHASE_NONE = 0,     /* before the stage: batches and stage callbacks are taken */
    KILN_REGISTRY_PHASE_ADD = 1,      /* the stage runs: every mod's add */
    KILN_REGISTRY_PHASE_ALTER = 2,    /* every mod's alter */
    KILN_REGISTRY_PHASE_FINAL = 3,    /* every mod's final */
    KILN_REGISTRY_PHASE_START = 4,    /* the stage ended: restart fields are final; live fields not applied (yet, or the
                                         world stopped) */
    KILN_REGISTRY_PHASE_WORLDLOAD = 5 /* a world loaded with every field applied */
};

/* read() / select(): which values. Permanent tags. */
enum {
    KILN_REGISTRY_VIEW_BASE = 0,    /* what the stage read: the game's data as the archive writer published it, before
                                       Kiln's older paths and the registry. From the stage's start. */
    KILN_REGISTRY_VIEW_STAGE = 1,   /* the resolved view as of the end of the previous stage (in add: the base with Kiln's
                                       older paths; in alter: after every add; in final: after every alter), never this
                                       stage's pending edits, the caller's own included: every mod's callback of one stage
                                       reads the same values, whatever the order. Only inside the caller's stage callback. */
    KILN_REGISTRY_VIEW_RESOLVED = 2 /* the registry's result: restart fields as baked; live fields as last applied (before
                                       the first world load: as the stage composed them). From START. */
};

/* KilnRegistryFieldInfo.mapping: the field on this game build. Permanent tags. */
enum {
    KILN_REGISTRY_MAPPED = 0,   /* mapped and verified for this build */
    KILN_REGISTRY_UNMAPPED = 1, /* Kiln has no mapping for this build yet (a Kiln update is needed): edits are refused */
    KILN_REGISTRY_UNPROVEN = 2, /* a mapping exists but is not verified for this build: edits are refused */
    KILN_REGISTRY_REMOVED = 3   /* the game build removed what the field maps to: edits are refused */
};

/* KilnRegistryFieldInfo.type: a field's value (read() gives it as JSON: a number, true/false, a name, an id, a list). */
enum {
    KILN_REGISTRY_TYPE_INT = 1,
    KILN_REGISTRY_TYPE_FLOAT = 2,
    KILN_REGISTRY_TYPE_BOOL = 3,
    KILN_REGISTRY_TYPE_ENUM = 4, /* one of the schema's names */
    KILN_REGISTRY_TYPE_ID = 5,   /* a Kiln id of another entry */
    KILN_REGISTRY_TYPE_LIST = 6  /* a list of plain values (list ops: clear, append, remove, replace) */
};

/* KilnRegistryFieldInfo.flags. Bits a reader does not know are ignored. */
enum { KILN_REGISTRY_FIELD_READ_ONLY = 1u << 0 };

/* KilnRegistryStatus.flags. Bits a reader does not know are ignored. */
enum {
    KILN_REGISTRY_STATUS_FAILED = 1u << 0 /* the stage did not run to its end (an internal error): nothing of it was applied;
                                             kiln.log and registry.json say why. The phase still moves on. */
};

typedef struct KilnRegistryStatus {
    uint32_t size;        /* in: sizeof(KilnRegistryStatus) of the caller (v1: 40); out: the bytes Kiln filled */
    uint32_t phase;       /* KILN_REGISTRY_PHASE_* */
    uint64_t generation;  /* 0 until the stage ended; 1 at START, +1 at each WORLDLOAD (each application of the live
                             fields: a world's load, a data-pack reload) */
    uint32_t edits;       /* edits the stage applied (every mod, every stage) */
    uint32_t refused;     /* edits refused (registry.json "refused") */
    uint32_t conflicts;   /* conflict lines (registry.json "conflicts") */
    uint32_t world;       /* v1: always KILN_WORLD_KINDS (the world that applied the live fields is not named yet) */
    uint32_t flags;       /* KILN_REGISTRY_STATUS_* */
    uint32_t reserved;    /* 0 */
} KilnRegistryStatus;

typedef struct KilnRegistryFieldInfo {
    uint32_t size;     /* in: sizeof(KilnRegistryFieldInfo) of the caller (v1: 192); out: the bytes Kiln filled */
    uint32_t applies;  /* KILN_REGISTRY_APPLIES_* */
    uint32_t mapping;  /* KILN_REGISTRY_MAPPED ... REMOVED, on this game build */
    uint32_t type;     /* KILN_REGISTRY_TYPE_* */
    uint32_t flags;    /* KILN_REGISTRY_FIELD_* */
    uint32_t reserved; /* 0 */
    char reason[168];  /* UTF-8, NUL-terminated, cut to fit: why the field is not MAPPED ("" when it is) */
} KilnRegistryFieldInfo;

typedef struct KilnRegistrySubmitResult {
    uint32_t size;     /* in: sizeof(KilnRegistrySubmitResult) of the caller (v1: 16); out: the bytes Kiln filled */
    uint32_t accepted; /* edits taken into the batch */
    uint32_t refused;  /* edits refused by the document's own checks (the report says why; the stage can refuse more:
                          an unmapped field, a conflict, an entry that is not there: registry.json "refused") */
    uint32_t reserved; /* 0 */
} KilnRegistrySubmitResult;

/* What a stage callback receives (valid only during the call). */
typedef struct KilnRegistryStageInfo {
    uint32_t size;  /* what Kiln filled (v1: 24) */
    uint32_t stage; /* KILN_REGISTRY_PHASE_ADD, ALTER or FINAL */
    KilnStr mod;    /* the mod whose turn it is (the subscribing owner's bound mod id); Kiln's copy */
} KilnRegistryStageInfo;

/* What a ready callback receives (valid only during the call). */
typedef struct KilnRegistryReady {
    uint32_t size;       /* what Kiln filled (v1: 24) */
    uint32_t phase;      /* KILN_REGISTRY_PHASE_START or WORLDLOAD */
    uint64_t generation; /* as KilnRegistryStatus.generation */
    uint32_t world;      /* v1: always KILN_WORLD_KINDS */
    uint32_t reserved;   /* 0 */
} KilnRegistryReady;

typedef void(KILN_CALL* KilnRegistryStageFn)(const KilnRegistryStageInfo* stage, void* user);
typedef void(KILN_CALL* KilnRegistryReadyFn)(const KilnRegistryReady* ready, void* user);

typedef struct KilnRegistryApi {
    KilnServiceHeader header; /* { sizeof(KilnRegistryApi), KILN_REGISTRY_VERSION } */

    /* Where the registry stands, as one snapshot. Any thread; never blocks for long. */
    KilnResult(KILN_CALL* status)(KilnRegistryStatus* out);

    /* The registry schemas (schema/registry/<registry>.json: registries, fields with type, range, unit, applies, ops,
     * docs) with each field's mapping on this game build ("mapping": "mapped" | "unmapped" | "unproven" | "removed",
     * "reason"). Any thread. */
    KilnResult(KILN_CALL* schema_json)(char* buf, size_t cap, size_t* out_len);

    /* One field: "<registry>.<field>" ("items.maxStack", "items.damage.fire"). KILN_ERR_NOT_FOUND: no such registry or
     * field in the schema. Any thread. */
    KilnResult(KILN_CALL* field_info)(KilnStr qualified_field, KilnRegistryFieldInfo* out);

    /* A mod's turn in each stage (the native form of C#'s Mod.OnRegistry; Kiln.Host uses it for every Kiln mod that
     * overrides OnRegistry): fn runs on the stage thread once in each of add, alter and final, at the owner's bound mod's
     * place in the order (see the stage above), after that mod's data files and queued batches. Inside fn: read/select with
     * VIEW_STAGE (the previous stage's resolved view), and submit batches of that stage for that mod. fn delays the game's
     * start while it runs: keep it short. One stage callback per mod: a second one for the same mod is
     * KILN_ERR_INVALID_ARGUMENT. Only before the stage (KILN_REGISTRY_PHASE_NONE: kiln_plugin_main or on_ready), else
     * KILN_ERR_UNAVAILABLE. Remove with unsubscribe. Any thread. */
    KilnResult(KILN_CALL* subscribe_stage)(KilnOwner owner, KilnRegistryStageFn fn, void* user, KilnHandle* out);

    /* One IR batch (schema/registry-ir.schema.json) of owner's. "mod" must be the owner's bound mod id: a batch naming
     * another mod is KILN_ERR_INVALID_ARGUMENT (nothing taken; the report and kiln.log name both; kiln_api.h has no separate
     * permission code). When:
     *  - before the stage (KILN_REGISTRY_PHASE_NONE), any thread: queued for its stage, at its mod's place;
     *  - during the stage, only inside the stage callback of the batch's mod, on the stage thread, and only a batch of
     *    the stage that runs: taken at once (applied when the callback returns). Anywhere else during the stage:
     *    KILN_ERR_WRONG_THREAD on another thread, KILN_ERR_UNAVAILABLE on the stage thread; a batch of another stage or
     *    mod: KILN_ERR_INVALID_ARGUMENT;
     *  - after the stage: KILN_ERR_UNAVAILABLE (a change at run time is a live change: registry P2's members, appended later).
     * KILN_OK: the document was read; out (optional) counts the edits taken and those refused by their own checks, the
     * report (optional) is {"accepted": n, "refused": [{"n", "reason"}]}. KILN_ERR_INVALID_ARGUMENT: the document is
     * unusable (not JSON, "ir" is not 1, no mod or stage, over KILN_REGISTRY_MAX_IR_BYTES): nothing of it is taken, the
     * report says why. */
    KilnResult(KILN_CALL* submit)(KilnOwner owner, KilnStr ir_json, KilnRegistrySubmitResult* out, char* report,
                                  size_t cap, size_t* out_len);

    /* One field of one entry in a view (KILN_REGISTRY_VIEW_*), as JSON (a number, true/false, a name, an id, a list).
     * registry: "items"; id: a Kiln id; field: a Kiln field path. KILN_ERR_NOT_FOUND: no such entry (or it was removed)
     * in that view. KILN_ERR_UNAVAILABLE: the view is not there yet (BASE before the stage, RESOLVED before START) or the
     * field is not mapped on this build. VIEW_STAGE: only inside the caller's stage callback (else WRONG_THREAD on
     * another thread, UNAVAILABLE on the stage thread). BASE and RESOLVED: any thread. */
    KilnResult(KILN_CALL* read)(uint32_t view, KilnStr registry, KilnStr id, KilnStr field, char* buf, size_t cap,
                                size_t* out_len);

    /* The ids a selector names in a view, as a JSON array in the registry's order. selector_json: an IR selector, either
     * a JSON string (an id, a case-insensitive glob over ids such as "game:material_*", "*", or an old debug name) or
     * {"id", "where", "has", "hasNot"}. Views and threads as read. */
    KilnResult(KILN_CALL* select)(uint32_t view, KilnStr registry, KilnStr selector_json, char* buf, size_t cap,
                                  size_t* out_len);

    /* The body of state/registry.json for the current generation (schema/state-registry.schema.json): every touched
     * field with its writers, the conflicts, the refused edits. KILN_ERR_UNAVAILABLE before START. Any thread. */
    KilnResult(KILN_CALL* report_json)(char* buf, size_t cap, size_t* out_len);

    /* fn runs at START, on the stage thread right after the stage, and at each WORLDLOAD (each application of the live
     * fields), on the thread that applied them (the tick thread for a server world, the game's main thread for a client world). Events
     * are not replayed: subscribe before the stage (kiln_plugin_main, on_ready) to
     * see START; a later subscriber reads status() after subscribing (an event between the two may reach fn too: treat a
     * generation seen twice as one). Remove with unsubscribe. Any thread. */
    KilnResult(KILN_CALL* subscribe_ready)(KilnOwner owner, KilnRegistryReadyFn fn, void* user, KilnHandle* out);

    /* Removes a subscription of subscribe_stage or subscribe_ready. Any thread, also inside the callback itself; once it
     * returned, fn is not called again, except a call already running on another thread. KILN_ERR_NOT_FOUND: unknown or
     * already removed. */
    KilnResult(KILN_CALL* unsubscribe)(KilnHandle handle);

} KilnRegistryApi;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#define KILN_REGISTRY_ASSERT static_assert
#else
#define KILN_REGISTRY_ASSERT _Static_assert
#endif
KILN_REGISTRY_ASSERT(sizeof(KilnRegistryStatus) == 40 && offsetof(KilnRegistryStatus, phase) == 4 &&
                         offsetof(KilnRegistryStatus, generation) == 8 && offsetof(KilnRegistryStatus, edits) == 16 &&
                         offsetof(KilnRegistryStatus, refused) == 20 && offsetof(KilnRegistryStatus, conflicts) == 24 &&
                         offsetof(KilnRegistryStatus, world) == 28 && offsetof(KilnRegistryStatus, flags) == 32 &&
                         offsetof(KilnRegistryStatus, reserved) == 36,
                     "KilnRegistryStatus v1 layout");
KILN_REGISTRY_ASSERT(sizeof(KilnRegistryFieldInfo) == 192 && offsetof(KilnRegistryFieldInfo, applies) == 4 &&
                         offsetof(KilnRegistryFieldInfo, mapping) == 8 && offsetof(KilnRegistryFieldInfo, type) == 12 &&
                         offsetof(KilnRegistryFieldInfo, flags) == 16 && offsetof(KilnRegistryFieldInfo, reserved) == 20 &&
                         offsetof(KilnRegistryFieldInfo, reason) == 24,
                     "KilnRegistryFieldInfo v1 layout");
KILN_REGISTRY_ASSERT(sizeof(KilnRegistrySubmitResult) == 16 && offsetof(KilnRegistrySubmitResult, accepted) == 4 &&
                         offsetof(KilnRegistrySubmitResult, refused) == 8 && offsetof(KilnRegistrySubmitResult, reserved) == 12,
                     "KilnRegistrySubmitResult v1 layout");
KILN_REGISTRY_ASSERT(sizeof(KilnRegistryStageInfo) == 24 && offsetof(KilnRegistryStageInfo, stage) == 4 &&
                         offsetof(KilnRegistryStageInfo, mod) == 8,
                     "KilnRegistryStageInfo v1 layout");
KILN_REGISTRY_ASSERT(sizeof(KilnRegistryReady) == 24 && offsetof(KilnRegistryReady, phase) == 4 &&
                         offsetof(KilnRegistryReady, generation) == 8 && offsetof(KilnRegistryReady, world) == 16 &&
                         offsetof(KilnRegistryReady, reserved) == 20,
                     "KilnRegistryReady v1 layout");
KILN_REGISTRY_ASSERT(sizeof(KilnRegistryApi) == 88 && offsetof(KilnRegistryApi, status) == 8 &&
                         offsetof(KilnRegistryApi, schema_json) == 16 && offsetof(KilnRegistryApi, field_info) == 24 &&
                         offsetof(KilnRegistryApi, subscribe_stage) == 32 && offsetof(KilnRegistryApi, submit) == 40 &&
                         offsetof(KilnRegistryApi, read) == 48 && offsetof(KilnRegistryApi, select) == 56 &&
                         offsetof(KilnRegistryApi, report_json) == 64 && offsetof(KilnRegistryApi, subscribe_ready) == 72 &&
                         offsetof(KilnRegistryApi, unsubscribe) == 80,
                     "KilnRegistryApi v1 layout (88 bytes, x64; P2 appends at the tail)");
#undef KILN_REGISTRY_ASSERT

#endif /* KILN_REGISTRY_H */
