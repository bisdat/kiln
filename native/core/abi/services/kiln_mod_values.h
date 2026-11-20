/* SPDX-License-Identifier: MIT
 * kiln.mod-values v1 (in-core service, since 0.2.0-dev.31): a mod's own per-character values (counters, currencies,
 * experience with levels) for native callers, over the same server ledger managed mods use (Kiln.ModValues,
 * docs/kiln/services/mod-values.md; dossier chapter 05b section 5). Not a second ledger: every call goes to the managed
 * backend Kiln.ModValues hands the core once at its start (kiln.mod-values.backend below). Until it has (before the
 * first service tick, with no .NET runtime, or an older Kiln.ModValues), describe, define, read, change and
 * read_synced answer KILN_ERR_UNAVAILABLE; owner_namespace, subscribe and unsubscribe work without it.
 *
 * Namespaces. Every value belongs to the namespace of the owner that defines it, and a qualified key names it as
 * "namespace:id" (canonical ASCII, exactly one colon). A managed mod's owner (the host names it "mod:<manifest id>")
 * has its validated lowercase manifest id as namespace, the same one Kiln.ModValues gives the mod. Every other owner
 * has "native." plus the first 32 lowercase hex digits of the SHA-256 of its exact registry owner name (the UTF-8
 * text it was created with, e.g. "plugin:my-mod.dll"; owner 0 is "kiln"). owner_namespace tells it. This keeps native
 * and managed namespaces apart by accident; it is no sandbox: native code can name any owner.
 *   namespace  ^[a-z0-9][a-z0-9._-]{0,63}$        id  ^[a-z][a-z0-9._-]{0,63}$
 *
 * Results (the one error rule). KILN_OK means a syntactically valid request got a domain answer: out->ok = 1, or
 * out->ok = 0 with out->reason (KMV_*; never KMV_NONE). Unknown keys (KMV_UNDEFINED), retired definitions
 * (KMV_DEFINITION_RETIRED), another namespace's value (KMV_ACCESS_DENIED) and the like are such answers, not errors.
 *   KILN_ERR_INVALID_ARGUMENT  a null pointer, a record size below v1, an unknown enum value, flag bit or operation,
 *                              bad key/id syntax, a definition the rules refuse (Currency + Carried, Carried without
 *                              KMV_TRUST_IMPORTS, read_mods outside KMV_READ_LIST, bad thresholds, bounds, scale,
 *                              labels or number formats), an amount the operation refuses (SPEND <= 0, a negative ADD
 *                              outside Counter, SPEND outside Currency)
 *   KILN_ERR_REVOKED           the owner was revoked (or never existed)
 *   KILN_ERR_UNAVAILABLE       no backend yet, or its capability is off here (stats.definitions, stats.own on the side
 *                              that runs the world, stats.sync on a client)
 *   KILN_ERR_WRONG_THREAD      define, read or change off the server's tick thread
 *   KILN_ERR_BUFFER_TOO_SMALL  owner_namespace only
 *   KILN_ERR_INTERNAL          the backend failed (logged); nothing was changed that the result does not show
 * The checks run in that order: arguments, owner, backend, capability, thread. On any error the result's payload is
 * zero (ok 0, reason 0, flags 0).
 *
 * Before and After. KilnModValueResult.flags says which snapshot is there (a value of 0 is a value, not absence):
 * refused: 0; read and read_synced: KMV_AFTER; an accepted change: KMV_BEFORE | KMV_AFTER (equal for a no-op such as
 * adding 0, which takes no revision and sends no event). The other snapshot is zero. Events: KMV_INITIALIZED has
 * KMV_AFTER only; a synced value that appeared or left has only the side it has.
 *
 * Characters. KilnModCharacter.account is the game's account key (not a Steam id); guid_rfc4122 is the game's
 * CharacterId in RFC 4122 (network, big-endian) byte order, as the GUID is written: 00112233-4455-6677-8899-aabbccddeeff
 * is the bytes 00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff, NOT .NET's Guid.ToByteArray mixed-endian form. World
 * GUIDs use the same order. An all-zero GUID with an account (an account-only key: this build gave no character id)
 * has no per-character identity: CharacterUnavailable. Account 0 with GUID zero is no character: INVALID_ARGUMENT. The
 * text form is CharacterKey.Text: 16 hex digits of the account, '.', the 32 hex digits of the GUID.
 *
 * Threads. owner_namespace, describe, subscribe, unsubscribe: any thread. define, read, change: the server's tick
 * thread on the side that runs the world (a dedicated server, a hosting client). read_synced: a client, any thread
 * (the player's synced copy). Callbacks: canonical events on the server's tick thread after the change was saved to
 * the ledger, synced events on the client's main thread after the whole update is in place.
 *
 * Records. Input records (KilnModValueDef) carry the caller's sizeof: below v1 is INVALID_ARGUMENT, a larger one is
 * accepted and the bytes past v1 are ignored. Output records (KilnModValueResult, KilnModDefinitionResult) carry the
 * caller's sizeof too (at least v1); Kiln fills the v1 fields, leaves bytes past v1 untouched and keeps size. Records
 * Kiln hands to a callback carry Kiln's sizeof. Strings are UTF-8 (ptr, len), not NUL-terminated; everything given to
 * define is copied before it returns. Everything handed to a callback is borrowed until it returns: copy to keep.
 *
 * Capability rows: stats.definitions (describe), stats.own (define, read, change; the side that runs the world),
 * stats.sync (read_synced; client). The service row kiln.mod-values is on wherever the core's services are on.
 * The table is append-only: check header.size before calling a member added later than v1.
 */
#ifndef KILN_MOD_VALUES_H
#define KILN_MOD_VALUES_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_MOD_VALUES_SERVICE "kiln.mod-values"
#define KILN_MOD_VALUES_VERSION 1u

/* Limits of a definition (Kiln.ModValues applies the exact rules; these bound what the core passes on). */
#define KMV_MAX_ID_BYTES 64u          /* a local id; a namespace too */
#define KMV_MAX_KEY_BYTES 129u        /* namespace ':' id */
#define KMV_MAX_THRESHOLDS 256u
#define KMV_MAX_READ_MODS 32u
#define KMV_MAX_JSON_BYTES 4096u      /* label_json, number_format_json (the definition as a whole: 4096 bytes) */
#define KMV_NAMESPACE_CAPACITY 65u    /* the longest namespace and its NUL */

/* KilnModValueDef.kind, KilnModValue.kind */
enum KilnModValueKind {
    KMV_COUNTER = 0,    /* a plain number; may go down (a Carried counter stays >= 0) */
    KMV_CURRENCY = 1,   /* never below minimum (>= 0), spent with KMV_SPEND, never carried between worlds */
    KMV_EXPERIENCE = 2, /* levels from xp_thresholds; >= 0, scale 0 */
};
/* KilnModValueDef.persistence */
enum KilnModPersistence {
    KMV_WORLD = 0,   /* this world's record of the character */
    KMV_CARRIED = 1, /* also travels with the character (maximum merge): experience or a nonnegative counter */
};
/* KilnModValueDef.read_policy: which namespaces may read (cooperative, not a sandbox) */
enum KilnModReadPolicy {
    KMV_OWNER_ONLY = 0,
    KMV_PUBLIC_READ = 1,
    KMV_READ_LIST = 2, /* the owner and read_mods */
};
/* KilnModValueDef.sync_policy: what the owning player's Kiln client receives */
enum KilnModSyncPolicy {
    KMV_SYNC_NONE = 0,      /* never sent */
    KMV_SYNC_OWNER_MOD = 1, /* sent; on the client only the defining namespace reads it */
    KMV_SYNC_READERS = 2,   /* sent; on the client the read policy decides */
};
/* KilnModValueDef.flags; any other bit is INVALID_ARGUMENT */
enum KilnModDefFlags {
    KMV_TRUST_IMPORTS = 1,        /* required for Carried: a client's copy may raise the value (maximum merge) */
    KMV_ALLOW_PARTITION_MOVE = 2, /* with a higher version: move a value between World and Carried */
    KMV_RESET_KIND_CHANGE = 4,    /* with a higher version: a kind change resets to the new initial */
    KMV_HAS_INITIAL = 8,          /* initial is given; clear: the minimum (0 is a legitimate explicit initial) */
};
/* change(operation) */
enum KilnModChangeOp {
    KMV_ADD = 0,   /* amount: a signed delta for a World Counter; >= 0 for Currency, Experience and Carried */
    KMV_SPEND = 1, /* Currency only, amount > 0; refused with KMV_INSUFFICIENT_FUNDS below the minimum */
    KMV_SET = 2,   /* amount is the absolute raw value (a Carried value only upwards) */
    KMV_RESET = 3, /* amount ignored: a World value back to its initial; Carried: KMV_EPOCH_REQUIRED */
};
/* change(request_flags); any other bit is INVALID_ARGUMENT */
enum KilnModRequestFlags {
    KMV_EXPECT_REVISION = 1, /* compare-and-set: expected must point to {world, revision}; revision 0 is valid. Clear:
                                expected is ignored and may be NULL */
};
/* KilnModValueResult.reason (stable numbers, never renumbered; the managed ModValueReasons have the same names) */
enum KilnModValueReason {
    KMV_NONE = 0,
    KMV_INSUFFICIENT_FUNDS = 1,
    KMV_STALE_REVISION = 2,       /* expected's world or revision differs */
    KMV_CHARACTER_UNAVAILABLE = 3, /* account-only key, no world, a write for a character not in game */
    KMV_BOUND_EXCEEDED = 4,
    KMV_UNDEFINED = 5,
    KMV_ACCESS_DENIED = 6,
    KMV_NOT_SYNCED = 7,           /* read_synced: no copy (not received, sync none, left the world) */
    KMV_DEFINITION_RETIRED = 8,   /* its owner unloaded or was revoked; the data is parked */
    KMV_MIGRATION_REQUIRED = 9,   /* the saved value is held for a higher definition version */
    KMV_IMPORT_REFUSED = 10,
    KMV_NON_MONOTONIC = 11,       /* carried progress only grows */
    KMV_BUDGET = 12,              /* 128 definitions per namespace, revisions exhausted */
    KMV_EPOCH_REQUIRED = 13,      /* carried progress resets only through a higher carry_epoch */
    KMV_NOT_READY = 14,           /* the character was not in game since the definition came */
    KMV_DEFINITION_CONFLICT = 15, /* describe/define: the same id with other content */
};
/* KilnModValueResult.flags */
enum KilnModResultFlags { KMV_BEFORE = 1, KMV_AFTER = 2 };
/* subscribe(event_scope), KilnModValueChange.event_scope */
enum KilnModEventScope {
    KMV_CANONICAL_EVENTS = 0, /* the server ledger's changes (server tick thread) */
    KMV_SYNCED_EVENTS = 1,    /* the client's synced copy (client main thread) */
};
/* KilnModValueChange.event_kind */
enum KilnModEventKind {
    KMV_INITIALIZED = 0,          /* a character got the value (AFTER only) */
    KMV_CHANGED = 1,              /* ADD, SPEND, SET; synced: a value that was there and is there */
    KMV_MIGRATED = 2,             /* a higher definition version converted the saved value */
    KMV_IMPORTED = 3,             /* a client's carried copy raised it */
    KMV_AVAILABILITY_CHANGED = 4, /* synced: a value appeared or left the copy (no claim of a saved write) */
    KMV_RESET_VALUE = 5,          /* RESET */
    KMV_EPOCH_RESET = 6,          /* a higher carry_epoch reset carried progress */
};

typedef struct KilnModValueVersion {
    uint8_t world_rfc4122[16];
    uint64_t revision; /* the value's revision in that world; 0 is a real revision (a fresh value) */
} KilnModValueVersion; /* 24 bytes */

typedef struct KilnModCharacter {
    uint64_t account;         /* the game's account key */
    uint8_t guid_rfc4122[16]; /* the game's CharacterId, RFC 4122 byte order */
} KilnModCharacter; /* 24 bytes */

typedef struct KilnModValueDef {
    uint32_t size;        /* the caller's sizeof (144) */
    uint32_t kind;        /* KMV_COUNTER / KMV_CURRENCY / KMV_EXPERIENCE */
    uint32_t persistence; /* KMV_WORLD / KMV_CARRIED */
    uint32_t scale;       /* fixed decimal places, 0..6 (experience: 0) */
    uint32_t flags;       /* KilnModDefFlags */
    uint32_t version;     /* >= 1; raise it when kind, bounds, scale, thresholds, initial or persistence change */
    uint32_t carry_epoch; /* >= 1; Carried: raise it to reset carried progress */
    uint32_t read_policy; /* KilnModReadPolicy */
    uint32_t sync_policy; /* KilnModSyncPolicy */
    KilnStr local_id;     /* the id in the caller's namespace */
    KilnStr label_json;   /* UiText as JSON: "text" (every language) or {"en": "...", "de": "...", "*": "..."} */
    KilnStr number_format_json; /* the same encoding; len 0: N<scale>. N0-N6, F0-F6, "0", "0." + 1..6 zeros */
    int64_t minimum, maximum;   /* raw bounds; Currency and Experience: minimum >= 0 */
    int64_t initial;            /* read only with KMV_HAS_INITIAL */
    const int64_t* xp_thresholds; /* Experience only (required there): 0 first, strictly increasing, at most 256 */
    uint32_t threshold_count;
    uint32_t reserved0;
    const KilnStr* read_mods;     /* KMV_READ_LIST only: namespaces that may read, at most 32 */
    uint32_t read_mod_count;
    uint32_t reserved1;
} KilnModValueDef; /* 144 bytes */

typedef struct KilnModValue {
    uint32_t size;               /* sizeof(KilnModValue) (72), also when the snapshot is absent */
    uint32_t kind, persistence, scale;
    uint32_t level;              /* Experience: how many thresholds the value has reached; 0 otherwise */
    uint32_t definition_version, carry_epoch;
    uint32_t reserved;
    int64_t value;               /* the raw value: value / 10^scale is what players see */
    uint64_t value_revision;     /* per value in this world; +1 per accepted change */
    uint64_t partition_revision; /* per character, namespace and partition */
    uint8_t world_rfc4122[16];   /* the world the snapshot is of */
} KilnModValue; /* 72 bytes */

typedef struct KilnModValueResult {
    uint32_t size;  /* the caller's sizeof (184) */
    uint32_t ok;    /* 1 accepted, 0 refused */
    uint32_t reason; /* KilnModValueReason when ok is 0 */
    uint32_t flags; /* KMV_BEFORE, KMV_AFTER: which snapshot is there */
    KilnModCharacter character; /* the character the result is about */
    KilnModValue before, after;
} KilnModValueResult; /* 184 bytes */

typedef struct KilnModDefinitionResult {
    uint32_t size, ok, reason, reserved; /* size: the caller's sizeof (16) */
} KilnModDefinitionResult;

typedef struct KilnModValueChange {
    uint32_t size;        /* sizeof(KilnModValueChange) (248) */
    uint32_t event_kind;  /* KilnModEventKind */
    uint32_t event_scope; /* KilnModEventScope */
    uint32_t reserved;
    uint64_t request_id;  /* the native caller's request id; 0: managed code or no request id */
    KilnModCharacter character;
    KilnStr qualified_key;
    KilnModValueResult result; /* ok 1; flags and before/after as above */
} KilnModValueChange; /* 248 bytes */

typedef void(KILN_CALL* KilnModValueChangedFn)(const KilnModValueChange* change, void* user);

typedef struct KilnModValuesApi {
    KilnServiceHeader header; /* { sizeof(KilnModValuesApi), KILN_MOD_VALUES_VERSION }; append-only */

    /* The owner's namespace as UTF-8 with a NUL. *required (not NULL) gets the bytes it needs, NUL included (at most
     * KMV_NAMESPACE_CAPACITY). capacity < *required: KILN_ERR_BUFFER_TOO_SMALL and nothing is written to utf8 (never a
     * truncated namespace); utf8 may be NULL with capacity 0 to ask. REVOKED for a revoked owner. Any thread. */
    KilnResult(KILN_CALL* owner_namespace)(KilnOwner owner, char* utf8, uint32_t capacity, uint32_t* required);

    /* Makes local_id known in the owner's namespace without activating it (labels, formats, kind for UI before any
     * server answers). Both sides, any thread (stats.definitions). The same definition again: ok; other content for the
     * same id: ok 0, KMV_DEFINITION_CONFLICT; past 128 definitions: KMV_BUDGET. */
    KilnResult(KILN_CALL* describe)(KilnOwner owner, const KilnModValueDef* def, KilnModDefinitionResult* out);

    /* Describes and activates local_id (stats.own, server tick thread). Characters in game get it at its initial value
     * now (a KMV_INITIALIZED event each), others when they are next in game. A higher version converts saved values; a
     * changed definition at the same version holds them (reads: KMV_MIGRATION_REQUIRED). */
    KilnResult(KILN_CALL* define)(KilnOwner owner, const KilnModValueDef* def, KilnModDefinitionResult* out);

    /* A character's value, also of a character that is offline (stats.own, server tick thread). Another namespace's
     * value where its read policy allows. Refused: KMV_UNDEFINED, KMV_DEFINITION_RETIRED, KMV_ACCESS_DENIED,
     * KMV_CHARACTER_UNAVAILABLE, KMV_NOT_READY, KMV_MIGRATION_REQUIRED. */
    KilnResult(KILN_CALL* read)(KilnOwner owner, const KilnModCharacter* character, KilnStr qualified_key,
                                KilnModValueResult* out);

    /* Changes a value of the owner's own namespace for a character in game (stats.own, server tick thread). request_id
     * is carried to the change's event (0: none). Refused (nothing changed): KMV_ACCESS_DENIED (another namespace),
     * KMV_UNDEFINED, KMV_DEFINITION_RETIRED, KMV_CHARACTER_UNAVAILABLE, KMV_STALE_REVISION, KMV_INSUFFICIENT_FUNDS,
     * KMV_BOUND_EXCEEDED, KMV_NON_MONOTONIC, KMV_EPOCH_REQUIRED, KMV_MIGRATION_REQUIRED, KMV_NOT_READY, KMV_BUDGET. */
    KilnResult(KILN_CALL* change)(KilnOwner owner, const KilnModCharacter* character, KilnStr qualified_key,
                                  uint32_t operation, int64_t amount, uint32_t request_flags,
                                  const KilnModValueVersion* expected, uint64_t request_id, KilnModValueResult* out);

    /* Client, any thread (stats.sync): this player's copy of a value as the server last sent it, read with the owner's
     * namespace (the defining namespace reads its own; others where the value's sync and read policy allow).
     * out->character is the local character. Refused: KMV_NOT_SYNCED, KMV_ACCESS_DENIED. */
    KilnResult(KILN_CALL* read_synced)(KilnOwner owner, KilnStr qualified_key, KilnModValueResult* out);

    /* fn(change, user) for each event of event_scope whose key passes qualified_filter: "namespace:id" (one value),
     * "namespace:*" (every value of a namespace) or empty (the owner's own namespace). Another namespace's events
     * arrive only while the value's read policy lets the owner read it (checked per event; for synced events its sync
     * policy must be KMV_SYNC_READERS too); a filter never grants a write. Any thread; works before the backend is
     * bound. A callback that faults is removed (logged). Revoking the owner removes its subscriptions. *out gets a
     * handle (not 0). INVALID_ARGUMENT: scope, filter syntax, no fn or out. */
    KilnResult(KILN_CALL* subscribe)(KilnOwner owner, uint32_t event_scope, KilnStr qualified_filter,
                                     KilnModValueChangedFn fn, void* user, KilnHandle* out);

    /* Any thread. When it returns, no call of fn is running (unless it is called from inside one on the same thread)
     * and none starts. NOT_FOUND: no such subscription (already removed, or its owner revoked). */
    KilnResult(KILN_CALL* unsubscribe)(KilnHandle handle);
} KilnModValuesApi; /* 72 bytes */

/* ====================================================================================================================
 * kiln.mod-values.backend v1: for Kiln.ModValues only (precedent kiln.ui.tick). The managed host hands the core its
 * process-lifetime backend functions once (bind), and delivers the ledger's events to native subscribers (dispatch).
 * Mods never call this table.
 *  - bind: the core copies *backend (size >= sizeof(KilnModValuesBackend), every function set). The first bind wins
 *    for the life of the process; a second one is INVALID_ARGUMENT (logged). Any thread.
 *  - The core calls the backend on the facade caller's thread, after its own checks, with the owner's validated
 *    namespace as owner_ns, records of Kiln's sizeof, and out zeroed with size set. change gets expected only with
 *    KMV_EXPECT_REVISION, else NULL. The backend fills ok, reason, character, flags, before and after; the core keeps
 *    the rules above (refused: flags 0; read: AFTER; accepted change: BEFORE|AFTER, else KILN_ERR_INTERNAL).
 *    A backend function never lets an exception out: it returns a KilnResult as above.
 *  - retire(owner_ns): the last owner that described or defined in owner_ns was revoked: its definitions leave the
 *    active view, its data is parked. On the revoking thread.
 *  - wanted(): KMV_WANT_* bits of the scopes with a native subscriber now (cheap, lock-free; any thread): the host
 *    builds and dispatches an event only when its bit is set.
 *  - dispatch(change, exported): one event to every matching subscriber, on the calling thread (canonical: server
 *    tick; synced: client main), synchronously. exported is the value's read and sync policy now (rechecked here for
 *    each subscriber of another namespace). INVALID_ARGUMENT for a bad record.
 * ================================================================================================================== */
#define KILN_MOD_VALUES_BACKEND_SERVICE "kiln.mod-values.backend"
#define KILN_MOD_VALUES_BACKEND_VERSION 1u

enum { KMV_WANT_CANONICAL = 1u << KMV_CANONICAL_EVENTS, KMV_WANT_SYNCED = 1u << KMV_SYNCED_EVENTS };

typedef struct KilnModValueExport {
    uint32_t size;           /* sizeof(KilnModValueExport) (24) */
    uint32_t read_policy;    /* KilnModReadPolicy */
    uint32_t sync_policy;    /* KilnModSyncPolicy */
    uint32_t read_mod_count; /* KMV_READ_LIST: entries in read_mods */
    const KilnStr* read_mods;
} KilnModValueExport; /* 24 bytes */

typedef struct KilnModValuesBackend {
    uint32_t size; /* sizeof(KilnModValuesBackend) (56) */
    uint32_t reserved;
    KilnResult(KILN_CALL* describe)(KilnStr owner_ns, const KilnModValueDef* def, KilnModDefinitionResult* out);
    KilnResult(KILN_CALL* define)(KilnStr owner_ns, const KilnModValueDef* def, KilnModDefinitionResult* out);
    KilnResult(KILN_CALL* read)(KilnStr owner_ns, const KilnModCharacter* character, KilnStr qualified_key,
                                KilnModValueResult* out);
    KilnResult(KILN_CALL* change)(KilnStr owner_ns, const KilnModCharacter* character, KilnStr qualified_key,
                                  uint32_t operation, int64_t amount, const KilnModValueVersion* expected,
                                  uint64_t request_id, KilnModValueResult* out);
    KilnResult(KILN_CALL* read_synced)(KilnStr owner_ns, KilnStr qualified_key, KilnModValueResult* out);
    void(KILN_CALL* retire)(KilnStr owner_ns);
} KilnModValuesBackend; /* 56 bytes */

typedef struct KilnModValuesBackendApi {
    KilnServiceHeader header; /* { sizeof(KilnModValuesBackendApi), KILN_MOD_VALUES_BACKEND_VERSION } */
    KilnResult(KILN_CALL* bind)(const KilnModValuesBackend* backend);
    uint32_t(KILN_CALL* wanted)(void);
    KilnResult(KILN_CALL* dispatch)(const KilnModValueChange* change, const KilnModValueExport* exported);
} KilnModValuesBackendApi; /* 32 bytes */

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#define KMV_ASSERT static_assert
#else
#define KMV_ASSERT _Static_assert
#endif
KMV_ASSERT(sizeof(KilnModValueVersion) == 24 && offsetof(KilnModValueVersion, revision) == 16, "KilnModValueVersion v1");
KMV_ASSERT(sizeof(KilnModCharacter) == 24 && offsetof(KilnModCharacter, guid_rfc4122) == 8, "KilnModCharacter v1");
KMV_ASSERT(sizeof(KilnModValueDef) == 144 && offsetof(KilnModValueDef, kind) == 4 &&
               offsetof(KilnModValueDef, persistence) == 8 && offsetof(KilnModValueDef, scale) == 12 &&
               offsetof(KilnModValueDef, flags) == 16 && offsetof(KilnModValueDef, version) == 20 &&
               offsetof(KilnModValueDef, carry_epoch) == 24 && offsetof(KilnModValueDef, read_policy) == 28 &&
               offsetof(KilnModValueDef, sync_policy) == 32 && offsetof(KilnModValueDef, local_id) == 40 &&
               offsetof(KilnModValueDef, label_json) == 56 && offsetof(KilnModValueDef, number_format_json) == 72 &&
               offsetof(KilnModValueDef, minimum) == 88 && offsetof(KilnModValueDef, maximum) == 96 &&
               offsetof(KilnModValueDef, initial) == 104 && offsetof(KilnModValueDef, xp_thresholds) == 112 &&
               offsetof(KilnModValueDef, threshold_count) == 120 && offsetof(KilnModValueDef, read_mods) == 128 &&
               offsetof(KilnModValueDef, read_mod_count) == 136,
           "KilnModValueDef v1 (x64)");
KMV_ASSERT(sizeof(KilnModValue) == 72 && offsetof(KilnModValue, level) == 16 &&
               offsetof(KilnModValue, definition_version) == 20 && offsetof(KilnModValue, carry_epoch) == 24 &&
               offsetof(KilnModValue, value) == 32 && offsetof(KilnModValue, value_revision) == 40 &&
               offsetof(KilnModValue, partition_revision) == 48 && offsetof(KilnModValue, world_rfc4122) == 56,
           "KilnModValue v1");
KMV_ASSERT(sizeof(KilnModValueResult) == 184 && offsetof(KilnModValueResult, flags) == 12 &&
               offsetof(KilnModValueResult, character) == 16 && offsetof(KilnModValueResult, before) == 40 &&
               offsetof(KilnModValueResult, after) == 112,
           "KilnModValueResult v1");
KMV_ASSERT(sizeof(KilnModDefinitionResult) == 16, "KilnModDefinitionResult v1");
KMV_ASSERT(sizeof(KilnModValueChange) == 248 && offsetof(KilnModValueChange, request_id) == 16 &&
               offsetof(KilnModValueChange, character) == 24 && offsetof(KilnModValueChange, qualified_key) == 48 &&
               offsetof(KilnModValueChange, result) == 64,
           "KilnModValueChange v1 (x64)");
KMV_ASSERT(sizeof(KilnModValuesApi) == 72 && offsetof(KilnModValuesApi, owner_namespace) == 8 &&
               offsetof(KilnModValuesApi, describe) == 16 && offsetof(KilnModValuesApi, define) == 24 &&
               offsetof(KilnModValuesApi, read) == 32 && offsetof(KilnModValuesApi, change) == 40 &&
               offsetof(KilnModValuesApi, read_synced) == 48 && offsetof(KilnModValuesApi, subscribe) == 56 &&
               offsetof(KilnModValuesApi, unsubscribe) == 64,
           "KilnModValuesApi v1 (x64)");
KMV_ASSERT(sizeof(KilnModValueExport) == 24 && offsetof(KilnModValueExport, read_mods) == 16, "KilnModValueExport v1 (x64)");
KMV_ASSERT(sizeof(KilnModValuesBackend) == 56 && offsetof(KilnModValuesBackend, describe) == 8 &&
               offsetof(KilnModValuesBackend, define) == 16 && offsetof(KilnModValuesBackend, read) == 24 &&
               offsetof(KilnModValuesBackend, change) == 32 && offsetof(KilnModValuesBackend, read_synced) == 40 &&
               offsetof(KilnModValuesBackend, retire) == 48,
           "KilnModValuesBackend v1 (x64)");
KMV_ASSERT(sizeof(KilnModValuesBackendApi) == 32 && offsetof(KilnModValuesBackendApi, bind) == 8 &&
               offsetof(KilnModValuesBackendApi, wanted) == 16 && offsetof(KilnModValuesBackendApi, dispatch) == 24,
           "KilnModValuesBackendApi v1 (x64)");
#undef KMV_ASSERT

#endif /* KILN_MOD_VALUES_H */
