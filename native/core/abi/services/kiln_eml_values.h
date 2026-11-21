/* SPDX-License-Identifier: MIT
 * DESIGN INPUT ONLY: kiln.eml.values v1, a Kiln-internal consumer/provider
 * contract. Provider: kiln-eml (GPL). Consumer: kiln-sfi (MIT); startup uses its
 * graph in-process. Neutral sized records and opaque tokens only: no Lua state,
 * mlua value, kfc type, Rust error, or native game pointer crosses this boundary.
 * Reentrancy/borrow modes follow the source operation; errors are returned as
 * retained values/tokens, never unwound across C. Mirror this in the MIT adapter.
 * Archive identity/view records are INCLUDED from their one canonical header,
 * never duplicated here. Seventeen members form a SEPARATE values service.
 * The separately gated detailed-error amendment does not grow this table.
 *
 * Kiln-internal: the typed-value seam of Kiln's interpreter for ShroudForge mods, not in
 * the SDK's headers and no mod API; making it public is a decision of its own.
 */
#ifndef SFI_TYPED_CONTAINER_DRAFT_H
#define SFI_TYPED_CONTAINER_DRAFT_H
#include "kiln_eml_archive.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_EML_VALUES_SERVICE "kiln.eml.values"
#define KILN_EML_VALUES_VERSION 1u
/* KilnEmlArchiveView and the registry-linked 160-byte ArchiveIdentity come
 * only from kiln_eml_archive.h. A layout test includes that header first to
 * reject any future second definition or obsolete standalone fixture copy. */

enum { KILN_VALUE_STARTUP = 0, KILN_VALUE_RUNTIME = 1 };
enum {
    KILN_VALUE_NIL = 1, KILN_VALUE_BOOLEAN = 2, KILN_VALUE_INTEGER = 3,
    KILN_VALUE_NUMBER = 4, KILN_VALUE_STRING = 5,
    KILN_VALUE_STRUCT_OWNED = 16, KILN_VALUE_STRUCT_MAPPED = 17,
    KILN_VALUE_ARRAY_OWNED = 18, KILN_VALUE_ARRAY_MAPPED = 19,
    KILN_VALUE_VARIANT_OWNED = 20, KILN_VALUE_VARIANT_MAPPED = 21,
    KILN_VALUE_ITERATOR = 22, KILN_VALUE_RESULT = 23, KILN_VALUE_ERROR = 24
};
enum { KILN_VALUE_FIELD = 1, KILN_VALUE_INDEX = 2 };
enum {
    KILN_VALUE_HANDLED = 1, KILN_VALUE_UNSUPPORTED = 2, KILN_VALUE_SEMANTIC_ERROR = 3
};
enum { KILN_VALUE_MUTATED = 1u << 0 };
enum {
    KILN_VALUE_INFO_LENGTH = 1u << 0, KILN_VALUE_INFO_DIRTY = 1u << 1,
    KILN_VALUE_INFO_SOURCE_LENGTH = 1u << 2
};
enum {
    KILN_VALUE_STATIC_ARRAY = 1u << 0, KILN_VALUE_DIRTY = 1u << 1,
    KILN_VALUE_RESOURCE_ROOT = 1u << 2, KILN_VALUE_LOADED_SNAPSHOT = 1u << 3
};
enum {
    KILN_VALUE_READ_VIEW = 0, KILN_VALUE_READ_LENGTH = 1,
    KILN_VALUE_READ_DIRTY = 2, KILN_VALUE_READ_STRINGIFY = 3,
    KILN_VALUE_READ_SOURCE_BYTES = 4, KILN_VALUE_READ_ITER_BEGIN = 5,
    KILN_VALUE_READ_ITER_NEXT = 6
};
enum {
    KILN_VALUE_OPEN_RESOURCE = 1, KILN_VALUE_OPEN_BYTES = 2,
    KILN_VALUE_OPEN_LUA_INPUT = 3, KILN_VALUE_OPEN_LOADED_SNAPSHOT = 4,
    KILN_VALUE_OPEN_DEFAULT = 5
};

/* Tokens are unforgeable, process-unique IDs, never pointers or reused IDs.
 * A node/lease is VM/phase-arena-owned, NOT mod-owner-owned. A Lua reference in
 * this VM keeps it alive even after the creating mod retires. Owner arguments
 * and creator_owner are attribution only, never access/lifetime gates. No grant
 * or require-transfer authorization API exists; Lua possession in the same VM
 * is the authority, and the frontend never transports tokens to another VM.
 * layout_revision is the immutable registry instance revision, not game.version.
 */
typedef struct KilnEmlValueIdentityV1 {
    uint32_t size, lua_phase;
    KilnOwner creator_owner; /* attribution, may already be retired */
    KilnHandle arena;
    uint64_t node_id, revision, layout_revision;
    KilnEmlArchiveIdentityV1 archive;
} KilnEmlValueIdentityV1;

typedef struct KilnEmlValueInfoV1 {
    uint32_t size, requested; /* 0=identity/class only, no logical borrow */
    KilnEmlValueIdentityV1 identity;
    uint32_t kind, flags, type_index, qualified_hash;
    /* flags.DIRTY valid ONLY with INFO_DIRTY; requested=0 is identity/class
     * only, never false-clean evidence or an eager recursive dirty walk. */
    uint64_t logical_len, source_len; /* valid only with their requested bits */
} KilnEmlValueInfoV1;

typedef struct KilnEmlValueSelectorV1 {
    uint32_t size, kind;
    uint64_t index; /* source-specific Lua usize conversion already performed */
    KilnStr field;
} KilnEmlValueSelectorV1;

/* A fixed reply: reserve the item/result/error token before logical mutation.
 * Unsupported is ONLY a valid live non-array object, never a closed/forged token.
 * Removed nil: item is a nonzero nil-node lease AND MUTATED; absence: item=0,
 * no MUTATED. Lua return arity remains exactly the source binding's contract.
 */
typedef struct KilnEmlValueResultV1 {
    uint32_t size, disposition, flags, arity;
    KilnHandle item, payload, error;
    uint64_t receiver_revision;
} KilnEmlValueResultV1;

/* RESOURCE resolves the phase's shared ResourceInfo root by key. Loaded roots
 * use a unique ResourceInfo instance token, NOT GUID-only interning. BYTES,
 * DEFAULT and LUA_INPUT create fresh roots. All bytes are copied/retained.
 * No mode grants stage enrollment, PATCH, archive write or live-resource CAS.
 */
typedef struct KilnEmlValueOpenV1 {
    uint32_t size, origin;
    KilnStr qualified_type;
    KilnAssetKey key;
    uint64_t source_instance, source_session;
    const void* bytes;
    size_t length;
    uint64_t lua_object;
} KilnEmlValueOpenV1;

/* Generic protected Lua-object access, not typed/layout interpretation.
 * GETs/LEN/STRINGIFY execute exactly once per request on creator worker.
 * COPY never executes a getter again. Temporary returned tokens live through
 * the call; GPL retains tokens needed by its graph/error results explicitly.
 */
enum {
    KILN_LUA_DESCRIBE = 0, KILN_LUA_GET_FIELD_VALUE = 1,
    KILN_LUA_GET_FIELD_STRING = 2, KILN_LUA_LENGTH = 3,
    KILN_LUA_RAW_SEQUENCE_INDEX = 4, KILN_LUA_SOURCE_TO_STRING = 5,
    KILN_LUA_MAKE_TYPED_PROXY = 6
};
enum {
    KILN_LUA_NIL = 0, KILN_LUA_BOOLEAN = 1, KILN_LUA_INTEGER = 2,
    KILN_LUA_NUMBER = 3, KILN_LUA_STRING = 4, KILN_LUA_TABLE = 5,
    KILN_LUA_TYPED_VALUE = 6, KILN_LUA_KNOWN_USERDATA = 7,
    KILN_LUA_OTHER_USERDATA = 8, KILN_LUA_OTHER = 9
};
enum { KILN_LUA_ADAPTER_NONE = 0, KILN_LUA_ADAPTER_GUID = 1,
       KILN_LUA_ADAPTER_CONTENT = 2, KILN_LUA_ADAPTER_RESOURCE = 3 };
typedef struct KilnEmlLuaAccessV1 {
    uint32_t size, operation;
    uint64_t object;
    int64_t index;
    KilnStr key;
    /* MAKE_TYPED_PROXY only: object is its provisional typed-value lease;
     * arena/node_id are neutral identity, index=0 and key empty. All other
     * operations set these two fields to zero. No kfc type crosses. */
    KilnHandle arena;
    uint64_t node_id;
} KilnEmlLuaAccessV1;
typedef struct KilnEmlLuaValueV1 {
    uint32_t size, kind;
    uint64_t object, bits, byte_length;
    KilnHandle typed_value;
    uint64_t error_object;
    uint32_t adapter_class, lease_disposition;
} KilnEmlLuaValueV1;
enum { KILN_LUA_LEASE_NONE = 0, KILN_LUA_LEASE_ADOPTED = 1, KILN_LUA_LEASE_REUSED = 2 };
/* MAKE_TYPED_PROXY: host interns by arena/node and returns actual Lua userdata's
 * opaque object token. New proxy ADOPTS the provisional lease (frontend __gc
 * closes it once); reused proxy returns REUSED and GPL closes the provisional.
 * Failure never adopts; GPL closes it. Identity-only value_info reentry allowed.
 * This provisional lease is distinct from the caller-owned result.item lease.
 *
 * Temporary reader tokens live until the outermost service-call scope ends,
 * including nested calls. GPL explicitly retains slot/result pins before that
 * scope ends and releases each pin once on the worker. Only graph/cache slots
 * that own LuaValue in Source pin actual proxies; never a node's own proxy or
 * every interned value for the whole VM. Frontend interning is a WEAK table.
 * GC in MAKE_TYPED_PROXY or ANY reader callback may reenter value_close for
 * another proxy: queue/defer that close, never mutate graph under the outer op;
 * outer call completes, then collect drains exactly once (required regression).
 * Result-copy never remakes a proxy.
 * All other operations use LEASE_NONE. Nil/string scalars need no typed proxy.
 */
/* Before EVERY callback invocation: check reader.size >= offsetof(member) +
 * sizeof(member), reader.version == 1 and member != NULL. Never read a pointer
 * past a truncated table, even a pointer used only for failure cleanup. Snapshot
 * the admitted callback table; retain its host/user lifetime until pending=0. */
typedef struct KilnEmlLuaReaderV1 {
    uint32_t size, version;
    void* user; /* host VM context, never Lua_State; stays until arena pending=0 */
    KilnResult (KILN_CALL *access)(void*, const KilnEmlLuaAccessV1*, KilnEmlLuaValueV1*);
    KilnResult (KILN_CALL *copy)(void*, uint64_t object, void*, size_t, size_t*);
    KilnResult (KILN_CALL *retain)(void*, uint64_t object);
    void (KILN_CALL *release)(void*, uint64_t object);
    void (KILN_CALL *wake)(void*, KilnHandle arena); /* any-thread signal, NO Lua */
} KilnEmlLuaReaderV1;

/* Separate service: header + seventeen pointers = 144 bytes. Every entry since sfi.
 * Category D: worker-local non-world graph operations, with the affinity below.
 * Normal service lookup only; no early-boot values export.
 *
 * Token kind       Lifetime / closer
 * arena identity   VM/phase arena; not a lease, cannot authorize or close
 * creator control  private phase-host token; arena_close + arena_collect
 * value            arena + retained Lua/graph refs; value_close releases one
 *                  issued lease. Caller-owned temporary/result item leases may
 *                  close after wrapping/copying while the proxy remains alive;
 *                  only the proxy-ADOPTED lease waits for its final __gc
 * iterator         arena + iterator Lua state; value_close drops retained refs
 * result/payload   immutable retained packet; value_close drops packet refs,
 *                  not independently returned item leases
 * error            retained packet + original host-error refs; value_close;
 *                  releases host tokens only on the creator worker
 * A package owner's retirement/release_owner has NO effect on any token: another
 * mod's first access through an already-held Lua reference must still succeed.
 * Only arena_close revokes all leases; worker drops wait for arena_collect.
 * VM host teardown must close/drain the arena BEFORE destroying its Lua VM.
 * Close is idempotent for an issued token, not a never-issued token (NOT_FOUND).
 * Cross-VM tokens are never transported/exposed by the MIT frontend; token
 * metadata and protected-reader objects are bound to one VM/phase arena.
 *
 * create/open/info/read/child/set/clone/export/array ops and collect:
 * creator interpreter worker only. No world/render/window thread waits for it.
 * close/arena_close: any-thread nonblocking retirement, never Lua or freeing
 * worker-owned graph off-worker. Closed arenas reject immediately; collect
 * drains on worker. Host reader/user survives until arena pending=0; normal
 * Lua VM teardown itself stays on that worker.
 *
 * All local edits are allowed with PATCH=false. SF's explicit Resource.data
 * assignment/create_resource/content and the startup pass's writer apply have separate gates.
 * No Lua longjmp, C++ exception or Rust panic crosses this C boundary.
 */
typedef struct KilnEmlValuesApiV1 {
    KilnServiceHeader header;
    /* Returns a creator-host CONTROL lease, not the public identity.arena ID.
     * The control is retained only by host bindings, never exposed to Lua.
     * It selects open routing, combined with the archive-view identity, not a
     * mod-owner permission. Same-VM Lua values require no transfer grant. */
    KilnResult (KILN_CALL *arena_create)(KilnOwner host_owner, KilnEmlArchiveView view,
        uint32_t lua_phase, const KilnEmlLuaReaderV1*, KilnHandle* arena);
    KilnResult (KILN_CALL *arena_close)(KilnOwner host_owner, KilnHandle arena, uint32_t* pending);
    KilnResult (KILN_CALL *arena_collect)(KilnOwner host_owner, KilnHandle arena, uint32_t* pending);
    KilnResult (KILN_CALL *value_open)(KilnOwner owner, KilnHandle arena, KilnEmlArchiveView owner_view,
        const KilnEmlValueOpenV1*, KilnEmlValueResultV1*);
    KilnResult (KILN_CALL *value_info)(KilnOwner owner, KilnHandle value, KilnEmlValueInfoV1*);
    KilnResult (KILN_CALL *value_read)(KilnOwner owner, KilnHandle value, uint32_t query, KilnEmlValueResultV1*);
    KilnResult (KILN_CALL *value_child)(KilnOwner owner, KilnHandle value,
        const KilnEmlValueSelectorV1*, KilnEmlValueResultV1*);
    KilnResult (KILN_CALL *value_set)(KilnOwner owner, KilnHandle value,
        const KilnEmlValueSelectorV1*, uint64_t lua_object, KilnEmlValueResultV1*);
    KilnResult (KILN_CALL *value_clone)(KilnOwner owner, KilnHandle value, KilnEmlValueResultV1*);
    /* selected_type empty: convert this already-validated graph's exact type.
     * Nonempty: source encode validation/clone against that selected type first.
     * Payload is retained bytes, not a second get/encode during size probing.
     */
    KilnResult (KILN_CALL *value_export)(KilnOwner owner, KilnHandle value,
        KilnStr selected_type, KilnEmlValueResultV1*);
    /* Closes ANY lease kind: value, iterator, immutable result/payload/error.
     * An item's successful returned lease transfers to the caller. Payload and
     * error leases are separate, caller-owned, not destroyed by closing item.
     * Closing a result releases only its retained child leases, never another
     * independently held item lease or other Lua reference. Idempotent issued-token close. */
    KilnResult (KILN_CALL *value_close)(KilnOwner owner, KilnHandle value);
    /* Immutable retained result only: no second lookup/stringify/pop/next. */
    KilnResult (KILN_CALL *value_result_copy)(KilnOwner owner, KilnHandle result,
        void* out, size_t capacity, size_t* required);
    KilnResult (KILN_CALL *value_array_push)(KilnOwner owner, KilnHandle array,
        uint64_t lua_object, KilnEmlValueResultV1*);
    KilnResult (KILN_CALL *value_array_insert)(KilnOwner owner, KilnHandle array,
        uint64_t index, uint64_t lua_object, KilnEmlValueResultV1*);
    KilnResult (KILN_CALL *value_array_pop)(KilnOwner owner, KilnHandle array, KilnEmlValueResultV1*);
    KilnResult (KILN_CALL *value_array_remove)(KilnOwner owner, KilnHandle array,
        uint64_t index, KilnEmlValueResultV1*);
    KilnResult (KILN_CALL *value_array_clear)(KilnOwner owner, KilnHandle array, KilnEmlValueResultV1*);
} KilnEmlValuesApiV1;

#ifdef __cplusplus
}
#define SFI_VALUE_ASSERT static_assert
#else
#define SFI_VALUE_ASSERT _Static_assert
#endif
SFI_VALUE_ASSERT(sizeof(KilnEmlArchiveIdentityV1) == 160, "canonical registry-linked archive identity");
SFI_VALUE_ASSERT(sizeof(KilnEmlValueIdentityV1) == 208, "value identity reuses canonical archive record");
SFI_VALUE_ASSERT(sizeof(KilnEmlValueInfoV1) == 248, "value info");
SFI_VALUE_ASSERT(sizeof(KilnEmlValueSelectorV1) == 32, "selector");
SFI_VALUE_ASSERT(sizeof(KilnEmlValueResultV1) == 48, "fixed result");
SFI_VALUE_ASSERT(sizeof(KilnEmlValueOpenV1) == 88, "open request");
SFI_VALUE_ASSERT(sizeof(KilnEmlLuaAccessV1) == 56, "Lua access and neutral proxy request");
SFI_VALUE_ASSERT(sizeof(KilnEmlLuaValueV1) == 56, "Lua descriptor");
SFI_VALUE_ASSERT(sizeof(KilnEmlLuaReaderV1) == 56, "protected reader, no transfer gate");
SFI_VALUE_ASSERT(sizeof(KilnEmlValuesApiV1) == 144, "separate seventeen-member values service");
#undef SFI_VALUE_ASSERT
#endif
