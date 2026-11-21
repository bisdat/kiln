/* SPDX-License-Identifier: MIT
 * kiln.eml.archive v1 -- immutable archive reads and stateless copied-value codecs.
 * GPL kiln-eml implements this table; MIT consumers CALL it, never link parser code.
 * Boundary: only neutral sized records, counted bytes and opaque issued tokens cross
 * C. No lua_State, mlua value, kfc type, Rust object or game pointer crosses. No Lua,
 * reader callback, panic, exception or longjmp may cross or re-enter this interface.
 * Mutable LOCAL graphs/protected Lua readers belong to kiln.eml.values, NOT here.
 *
 * Kiln-internal: the seam of Kiln's interpreter for ShroudForge mods into kiln-eml's startup
 * pass. Not in the SDK's headers and no mod API; making it public is a decision of its own.
 *
 * Category D, both sides, any thread: no engine memory access, world/window dispatch,
 * callback or worker-local Lua cache. Calls may do archive IO on the caller's thread;
 * engine/render/window callbacks must not do that IO. No world thread waits on Lua.
 * Readers pin immutable storage while in flight. release/owner_revoke linearize with
 * new calls; an already pinned call may finish, and the last pin reclaims storage.
 * release never waits on the interpreter. No locks are held across external calls.
 *
 * A PREPARED/PUBLISHED generation issued by kiln-eml's startup pass is required; no paths, physical
 * stage/commit/abort or admission authority are exported. PUBLISHED is not a Lua phase.
 * A view binds owner (acquire argument), launch/plan UUIDs, reload epoch, archive and
 * registry identity/digests. All expected fields must match; no wildcard pins. Registry
 * ids/generations/tokens are nonzero, never pointers or reused process-local values.
 * New publication does not retarget a view. Revocation/epoch retirement invalidates it.
 * Runtime grants are PUBLISHED only; PREPARED grants require the startup pass's authorization.
 * identity is a copy of the granted pins, not a live global identity query.
 *
 * Tokens: release is idempotent for issued tokens, including after owner revocation;
 * an unknown/zero token is INVALID_ARGUMENT. Other uses of closed/stale/foreign tokens
 * are REVOKED. owner_revoke closes all that owner's views. A copied token grants no
 * other owner access (owner arguments must equal the grant's owner). Prefix methods
 * have no owner argument: possession is the issued owner's read/release authority.
 *
 * Record rules: size is caller-set, at least the v1 size. New fields append only;
 * larger sizes are accepted, bytes past v1 untouched. schema_version must be 1 where
 * present; unknown versions/flags/enums/reserved nonzero fields are INVALID_ARGUMENT.
 * KilnAssetKey/KilnContentHash reuse existing fixed 24/16-byte records, not new headers.
 * Input/output buffers and size/required pointers must not overlap; no pointer is
 * retained after return. Null data is allowed only with zero length/capacity. required
 * is mandatory. On BUFFER_TOO_SMALL set required to the COMPLETE byte/element count,
 * write nothing else, consume nothing. On OK set required to the complete count.
 * Other failures leave data outputs unchanged; a valid ErrorOut is always initialized.
 * The exception is the new per-call detailed-error record below. JSON is counted UTF-8 WITHOUT a NUL;
 * arbitrary binary output includes embedded zeros. No partial reads/truncation.
 * Arithmetic (first+limit, start+count, stride*count, record lengths) is checked before
 * access; overflow/out-of-range is INVALID_ARGUMENT. No ABI-wide result-size cap.
 *
 * Errors retain KilnResult meanings: NOT_FOUND = missing resource/content/type or
 * selector with no match; UNAVAILABLE = phase/policy/provider/unsupported type or
 * ambiguous selector; REVOKED = stale grant; INVALID_ARGUMENT = malformed query/tree,
 * type mismatch, invalid range/record; INTERNAL = contained provider fault. No SF
 * error strings are invented from these codes. A non-null ErrorOut preserves the
 * provider's original counted diagnostic. Translators still own SF result/raise shape.
 *
 * Capabilities (kiln.caps), distinct from kiln.eml.values. Each group is AVAILABLE
 * only when EVERY named member is implemented and can succeed for supported inputs:
 * eml.archive.metadata = acquire, identity, release, registry_metadata;
 * eml.archive.read = resource_json, find_resource, list_resources, read_resource,
 *                    content_read_range, list_contents;
 * eml.archive.codec = value_decode, value_encode, loaded_field;
 * eml.archive.attributes = attribute_catalog, attribute_resolve, attribute_evaluate.
 * eml.archive.errors = error_copy, error_release (precise-error channel only).
 * Precise SF callers additionally need their actual family/group/member facts
 * and source-cause emission/roundtrip proof; the errors row alone is not parity.
 * Metadata is independent: its 48-byte minimum does not require resource_json or
 * any read/codec/attribute slot. The read row stays UNAVAILABLE until ALL six read
 * members work; partial codec/attribute groups likewise stay UNAVAILABLE. Each
 * implemented member additionally reports <group>.<member>; those rows supplement,
 * NEVER replace or weaken, the complete-group guarantee. Unimplemented members are
 * null or return UNAVAILABLE, never fake success. A consumer checks version, member
 * extent, non-null, group AND member rows before each use. A held/off/absent row
 * refuses use. Static table support is separate from generation readiness (no
 * authorized generation -> UNAVAILABLE, not an empty successful reply). Early boot
 * before caps publication checks the same facts, as the startup pass supplies them;
 * getter presence alone never grants a view or support.
 *
 * JSON payloads are ABI: the schemas in schema/sfi/eml-archive/ (schema_version=1),
 * synthetic fixtures in schema/sfi/eml-archive/fixtures/. All 64-bit integers and float
 * bits use tagged strings, never JSON doubles. Indices/hash domains/order/default
 * presence must survive unchanged. See docs/kiln/services/eml-archive.md for schemas,
 * source-backed success paths, discovery and a generic native consumer example.
 */
#ifndef KILN_EML_ARCHIVE_H
#define KILN_EML_ARCHIVE_H
#include "../kiln_api.h"
#include "kiln_assets.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_EML_ARCHIVE_SERVICE "kiln.eml.archive"
#define KILN_EML_ARCHIVE_VERSION 1u
#define KILN_EML_ARCHIVE_SERVICE_EXPORT "kiln_eml_archive_service"
#define KILN_EML_ARCHIVE_METADATA_CAP "eml.archive.metadata"
#define KILN_EML_ARCHIVE_READ_CAP "eml.archive.read"
#define KILN_EML_ARCHIVE_CODEC_CAP "eml.archive.codec"
#define KILN_EML_ARCHIVE_ATTRIBUTES_CAP "eml.archive.attributes"
#define KILN_EML_ARCHIVE_ERRORS_CAP "eml.archive.errors"
#define KILN_EML_ARCHIVE_SCHEMA_VERSION 1u
#define KILN_EML_ARCHIVE_PREFIX_SIZE 40u
#define KILN_EML_ARCHIVE_REGISTRY_SUBSET_SIZE 48u

typedef uint64_t KilnEmlArchiveView;
enum { KILN_EML_ARCHIVE_PREPARED = 1, KILN_EML_ARCHIVE_PUBLISHED = 2 };
/* UUID bytes are RFC 4122/network order, unlike asset GUID bytes (game order). */
typedef struct KilnEmlArchiveIdentityV1 {
    uint32_t size;
    uint32_t phase;
    uint8_t launch_id[16];
    uint8_t plan_id[16];
    uint64_t generation;
    uint64_t reload_epoch;
    uint8_t plan_sha256[32];
    uint8_t archive_sha256[32];
    uint64_t registry_id;
    uint8_t registry_sha256[32];
} KilnEmlArchiveIdentityV1;

enum {
    KILN_ARCHIVE_FILTER_GUID = 1u,
    KILN_ARCHIVE_FILTER_TYPE = 2u,
    KILN_ARCHIVE_FILTER_PART = 4u,
    KILN_ARCHIVE_ORDER_CATALOG = 0u,
    KILN_ARCHIVE_ORDER_PART = 1u
};
/* flags select key fields, conjunctively; unselected fields are zero.
 * PART order requires GUID+TYPE filters and returns ascending part numbers.
 * CATALOG preserves source catalog order. limit=0 means the entire remaining suffix.
 * first (list_resources argument) indexes FILTERED results, not a native type index.
 * first==total is empty success; first>total or first+nonzero limit overflow is bad.
 * A nonzero limit selects min(limit,total-first), never silently clamped by capacity. */
typedef struct KilnArchiveQuery {
    uint32_t size;
    uint32_t schema_version;
    uint32_t flags;
    uint32_t order;
    KilnAssetKey key;
    uint64_t limit;
} KilnArchiveQuery;

/* No retained strings/handles. type_index is the ORIGINAL registry index.
 * source is FILE or TRANSACTION (never loaded-runtime); byte_length includes tails. */
enum { KILN_ARCHIVE_SOURCE_FILE = 1u, KILN_ARCHIVE_SOURCE_TRANSACTION = 2u };
typedef struct KilnArchiveResourceInfo {
    uint32_t size;
    uint32_t schema_version;
    KilnAssetKey key;
    uint32_t source;
    uint32_t reserved;
    uint64_t type_index;
    uint64_t byte_length;
    uint64_t generation;
} KilnArchiveResourceInfo;

/* A scalar selection in OWNED copied bytes, not a live engine address or write grant.
 * offset+width must lie in snapshot_length; width=1/2/4/8 and naturally aligned.
 * Assets must revalidate the selection and entire dynamic path against its live
 * transaction before CAS. Field lookup alone cannot authorize a write. */
enum {
    KILN_ARCHIVE_SCALAR_BOOL = 1u, KILN_ARCHIVE_SCALAR_SIGNED = 2u,
    KILN_ARCHIVE_SCALAR_UNSIGNED = 3u, KILN_ARCHIVE_SCALAR_FLOAT = 4u
};
typedef struct KilnFieldSelection {
    uint32_t size;
    uint32_t schema_version;
    uint64_t offset;
    uint32_t width;
    uint32_t scalar_kind;
    uint64_t type_index;
    uint64_t root_type_index;
    uint64_t registry_id;
    uint64_t snapshot_length;
    uint32_t flags; /* v1: zero */
    uint32_t reserved;
} KilnFieldSelection;

/* Binary tagged value tree v1. This is a NEW neutral transport, NOT a game/parser
 * layout. All serialized integers are little endian; read bytes without casting
 * potentially unaligned records. Each node starts with the 48-byte prefix below:
 * size=48 (header bytes), schema_version=1, flags=reserved=0, record_bytes=entire
 * subtree length. Then type_bytes UTF-8 qualified type name (no NUL), payload_bytes
 * raw bytes, then child_count complete child subtrees. No padding, offsets, pointers,
 * backreferences or trailing bytes. Root record_bytes equals the input length.
 * Unknown tags/versions, invalid UTF-8 type, wrong payload/child arity, incomplete
 * or overflowing sums are INVALID_ARGUMENT. Larger future headers need a new schema
 * version; the tree's serialized prefix is fixed, unlike caller-sized C records.
 *
 * NIL: 0 payload/children. BOOL: 1 byte 0/1. I64/U64/F64: 8 bytes, two's-complement/
 * unsigned/raw IEEE bits. F32: 4 raw IEEE bits (NaNs, signed zero preserved).
 * GUID/REFERENCE: 16 game-order GUID bytes; reference is an identity, NEVER an address.
 * BYTES/STRING: counted arbitrary bytes (including NUL/non-UTF8 Lua strings).
 * ARRAY: no payload, N children. MAP: no payload, even child count, ordered key/value
 * pairs (struct field keys are STRING); order/duplicate keys are preserved, not sorted.
 * VARIANT: 8-byte unsigned discriminant, exactly 1 child; its child's type name
 * identifies the selected type. OPTIONAL: 1-byte presence 0/1, respectively 0/1 child.
 * type_bytes may be zero for an untyped scalar; codecs validate typed names against
 * the view's registry. Nonfinite values are transportable, not a live-write approval.
 * Stateless traversal materializes a tree; persistent aliases/lazy Lua behavior
 * require kiln.eml.values and are not established by these two codec members.
 */
enum {
    KILN_ARCHIVE_VALUE_NIL = 0u, KILN_ARCHIVE_VALUE_BOOL = 1u,
    KILN_ARCHIVE_VALUE_I64 = 2u, KILN_ARCHIVE_VALUE_U64 = 3u,
    KILN_ARCHIVE_VALUE_F32 = 4u, KILN_ARCHIVE_VALUE_F64 = 5u,
    KILN_ARCHIVE_VALUE_GUID = 6u, KILN_ARCHIVE_VALUE_BYTES = 7u,
    KILN_ARCHIVE_VALUE_STRING = 8u, KILN_ARCHIVE_VALUE_ARRAY = 9u,
    KILN_ARCHIVE_VALUE_MAP = 10u, KILN_ARCHIVE_VALUE_VARIANT = 11u,
    KILN_ARCHIVE_VALUE_OPTIONAL = 12u, KILN_ARCHIVE_VALUE_REFERENCE = 13u
};
typedef struct KilnArchiveValueRecordV1 {
    uint32_t size;
    uint32_t schema_version;
    uint32_t tag;
    uint32_t flags;
    uint64_t record_bytes;
    uint64_t payload_bytes;
    uint64_t child_count;
    uint32_t type_bytes;
    uint32_t reserved;
} KilnArchiveValueRecordV1;

/* Per-call detailed-error transport (an amendment of v1, not a last-error).
 * Caller sets size and flags=reserved=0; extent/overlap/flags are checked BEFORE
 * any lookup/IO. Initialize the entire valid v1 record on EVERY return; preserve
 * the caller's size/unknown suffix. Success or capacity-only refusal has token=0,
 * message_bytes=0. A genuine provider failure captures its complete original cause
 * ONCE and issues a fresh immutable token even for raw capacity0/count0 calls.
 * NULL explicitly opts out of detail; it is not exact Source message parity.
 *
 * error_copy/error_release belong to the view's transport-root owner. That view
 * and owner stay alive through capture/copy/release; caller-held in-flight pins
 * survive close, while view/owner retirement refuses new calls and reclaims
 * retained transport state. Never use a creator-mod retirement gate for a job
 * another mod still references in the same VM. Copy text into owned job/Lua state
 * before closing the native token/transport. No callback enters Lua here.
 *
 * Full counted UTF-8 diagnostic bytes preserve embedded NUL; no terminator,
 * truncation, TLS/global last error or second IO/getter/codec to retrieve text.
 * error_copy short/probe writes nothing but full required, consumes nothing and
 * never overwrites the original outcome. error_release is idempotent for this
 * owner's issued tokens (even after retirement); unknown/zero INVALID_ARGUMENT,
 * foreign-owner REVOKED. ErrorOut flags/reserved are zero in v1.
 *
 * Known-length raw-read/content cap<required uses index metadata BEFORE IO.
 * required0/cap0 is NOT a sizing shortcut: Source open/seek/validation still runs
 * and may fail. A readonly view does NOT freeze files other DLLs/Steam can write.
 * Unknown-size JSON/attributes need owned generation-pinned materialization
 * before sizing/copy, with actual pinning proof, never a second live file read.
 * Stateless codec probes use the same caller-owned snapshot; live Lua/getters
 * use kiln.eml.values' once-only reader. Provider tests must prove these rules.
 *
 * Precise-error callers require eml.archive.errors AND their actual family/group
 * and member facts plus both copy/release extents/non-null. The errors row alone
 * never proves a family's Source cause emission or message parity. Native NULL
 * opt-out callers may use a complete family without claiming precise text.
 */
typedef struct KilnArchiveErrorV1 {
    uint32_t size;
    KilnResult result;
    KilnHandle token;
    uint64_t message_bytes;
    uint32_t flags;
    uint32_t reserved;
} KilnArchiveErrorV1;

typedef struct KilnEmlArchiveApiV1 {
    KilnServiceHeader header;
    /* Four-slot OFFSETS remain 8/16/24/32 (40-byte extent), NOT old call ABI.
     * This amendment changes resource_json and 12 other typedefs.
     * Re-gate/regenerate/recompile ALL callers, including the early getter,
     * the providers and fakes. acquire/identity/release stay unchanged. The existing
     * kiln.eml v1/v2 is NOT changed by this separate service. */
    KilnResult(KILN_CALL* acquire)(KilnOwner owner, const KilnEmlArchiveIdentityV1* expected, KilnEmlArchiveView* out);
    KilnResult(KILN_CALL* identity)(KilnEmlArchiveView view, KilnEmlArchiveIdentityV1* out);
    /* kiln.boot query grammar (boot_service.cpp:210-251):
     * resource:<lowercase canonical GUID>/<8 hex qualified type hash>/<decimal part>
     * (part<=INT32_MAX, no leading zeros); skill:0x<8 hex digits>;
     * item/template:<1..128 ASCII alnum/underscore/dot/hyphen, no '..'>;
     * material:<1..128 ASCII alnum/underscore>. No NUL/traversal; full schema envelope. */
    KilnResult(KILN_CALL* resource_json)(KilnEmlArchiveView view, KilnStr query, char* json, size_t capacity, size_t* required, KilnArchiveErrorV1* error);
    KilnResult(KILN_CALL* release)(KilnEmlArchiveView view);
    /* since sfi -- immutable read subset. limit=0: header only (types=[]).
     * first/limit are ordered registry ordinals, type.index remains native index.
     * first==count: empty; first>count or first+limit overflow: INVALID_ARGUMENT. */
    KilnResult(KILN_CALL* registry_metadata)(KilnEmlArchiveView view, uint64_t first, uint32_t limit,
                                          char* json, size_t capacity, size_t* required, KilnArchiveErrorV1* error);
    /* since sfi -- find/read include finalized staged entries in this generation;
     * list is reference-catalog enumeration (not new-only transaction entries). */
    KilnResult(KILN_CALL* find_resource)(KilnOwner owner, KilnEmlArchiveView archive, const KilnAssetKey* key,
                                      KilnArchiveResourceInfo* out, KilnArchiveErrorV1* error);
    /* capacity/required in ELEMENTS; fixed sizeof(KilnAssetKey) stride. */
    KilnResult(KILN_CALL* list_resources)(KilnOwner owner, KilnEmlArchiveView archive, const KilnArchiveQuery* query,
                                       uint64_t first, KilnAssetKey* out, size_t capacity, size_t* required, KilnArchiveErrorV1* error);
    KilnResult(KILN_CALL* read_resource)(KilnOwner owner, KilnEmlArchiveView archive, const KilnAssetKey* key,
                                      void* out, size_t capacity, size_t* required, KilnArchiveErrorV1* error);
    /* Game-reference contents only, never new staged content or arbitrary files.
     * Exactly count bytes: start<=hash.size and count<=hash.size-start; zero count is
     * valid at EOF. required=count, including on BUFFER_TOO_SMALL. */
    KilnResult(KILN_CALL* content_read_range)(KilnOwner owner, KilnEmlArchiveView archive, const KilnContentHash* hash,
                                           uint64_t start, uint64_t count, void* out, size_t capacity, size_t* required, KilnArchiveErrorV1* error);
    /* Entire reference-catalog suffix from first, capacity/required in ELEMENTS,
     * fixed sizeof(KilnContentHash) stride. No capacity-driven partial pages. */
    KilnResult(KILN_CALL* list_contents)(KilnOwner owner, KilnEmlArchiveView archive, uint64_t first,
                                      KilnContentHash* out, size_t capacity, size_t* required, KilnArchiveErrorV1* error);
    /* since sfi -- stateless neutral-tree codecs; registry is an issued view. */
    KilnResult(KILN_CALL* value_decode)(KilnOwner owner, KilnEmlArchiveView registry, KilnStr qualified_type,
                                     const void* bytes, size_t length, void* tagged_tree, size_t capacity, size_t* required, KilnArchiveErrorV1* error);
    KilnResult(KILN_CALL* value_encode)(KilnOwner owner, KilnEmlArchiveView registry, KilnStr qualified_type,
                                     const void* tagged_tree, size_t length, void* bytes, size_t capacity, size_t* required, KilnArchiveErrorV1* error);
    KilnResult(KILN_CALL* loaded_field)(KilnOwner owner, KilnEmlArchiveView registry, KilnStr qualified_type, KilnStr path,
                                     const void* bytes, size_t length, KilnFieldSelection* selection, KilnArchiveErrorV1* error);
    /* since sfi -- immutable metadata/calculation over caller-owned scalar bits.
     * selector_json has its OWN versioned schema (name/id selector, no engine token).
     * evaluate count is the complete root length; output_bits has count elements.
     * trace_json may be probed with capacity=0; BUFFER_TOO_SMALL also leaves ALL
     * output_bits untouched. Input/output may not alias. No engine reads/writes. */
    KilnResult(KILN_CALL* attribute_catalog)(KilnOwner owner, KilnEmlArchiveView registry,
                                          char* json, size_t capacity, size_t* required, KilnArchiveErrorV1* error);
    KilnResult(KILN_CALL* attribute_resolve)(KilnOwner owner, KilnEmlArchiveView registry, KilnStr selector_json,
                                          char* json, size_t capacity, size_t* required, KilnArchiveErrorV1* error);
    KilnResult(KILN_CALL* attribute_evaluate)(KilnOwner owner, KilnEmlArchiveView registry, uint32_t root_hash,
                                           const uint32_t* input_bits, uint32_t count, uint32_t* output_bits,
                                           char* trace_json, size_t capacity, size_t* required, KilnArchiveErrorV1* error);
    /* since sfi -- pure immutable error transport, never IO/codec replay. */
    KilnResult(KILN_CALL* error_copy)(KilnOwner owner, KilnHandle token,
                                    void* out, size_t capacity, size_t* required);
    KilnResult(KILN_CALL* error_release)(KilnOwner owner, KilnHandle token);
} KilnEmlArchiveApiV1;

/* Early getter and later normal service lookup MUST return the same process-lifetime
 * immutable table. min_version<=1 -> table, >1 -> NULL; neither path grants a view.
 * Resolve this export from the already-loaded GPL module; no new DLL is loaded.
 * This declaration does not export/implement it in MIT or change kiln.eml v1/v2. */
const KilnEmlArchiveApiV1* KILN_CALL kiln_eml_archive_service(uint32_t min_version);
typedef const KilnEmlArchiveApiV1*(KILN_CALL* KilnEmlArchiveServiceFn)(uint32_t min_version);

/* Short circuit BEFORE reading a tail pointer. api must be a stable pointer expression.
 * This tests structure only; it deliberately does not replace capability checks. */
#define KILN_EML_ARCHIVE_HAS(api, member) \
    ((api) != NULL && ((const KilnEmlArchiveApiV1*)(api))->header.size >= \
         offsetof(KilnEmlArchiveApiV1, member) + sizeof(((KilnEmlArchiveApiV1*)0)->member) && \
     ((const KilnEmlArchiveApiV1*)(api))->header.version >= KILN_EML_ARCHIVE_VERSION && \
     ((const KilnEmlArchiveApiV1*)(api))->member != NULL)

#ifdef __cplusplus
}
#define KILN_EML_ARCHIVE_ASSERT(c) static_assert(c, #c)
#define KILN_EML_ARCHIVE_ALIGN(t) alignof(t)
#else
#define KILN_EML_ARCHIVE_ASSERT(c) _Static_assert(c, #c)
#define KILN_EML_ARCHIVE_ALIGN(t) _Alignof(t)
#endif
#define KILN_EML_ARCHIVE_FIELD(t, f, o) KILN_EML_ARCHIVE_ASSERT(offsetof(t, f) == o)
#define KILN_EML_ARCHIVE_LAYOUT(t, s, a) \
    KILN_EML_ARCHIVE_ASSERT(sizeof(t) == s); KILN_EML_ARCHIVE_ASSERT(KILN_EML_ARCHIVE_ALIGN(t) == a)
#if UINTPTR_MAX == UINT64_MAX
KILN_EML_ARCHIVE_LAYOUT(KilnEmlArchiveIdentityV1, 160, 8);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveIdentityV1, size, 0);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveIdentityV1, phase, 4);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveIdentityV1, launch_id, 8);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveIdentityV1, plan_id, 24);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveIdentityV1, generation, 40);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveIdentityV1, reload_epoch, 48);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveIdentityV1, plan_sha256, 56);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveIdentityV1, archive_sha256, 88);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveIdentityV1, registry_id, 120);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveIdentityV1, registry_sha256, 128);
KILN_EML_ARCHIVE_LAYOUT(KilnArchiveQuery, 48, 8);
KILN_EML_ARCHIVE_FIELD(KilnArchiveQuery, size, 0);
KILN_EML_ARCHIVE_FIELD(KilnArchiveQuery, schema_version, 4);
KILN_EML_ARCHIVE_FIELD(KilnArchiveQuery, flags, 8);
KILN_EML_ARCHIVE_FIELD(KilnArchiveQuery, order, 12);
KILN_EML_ARCHIVE_FIELD(KilnArchiveQuery, key, 16);
KILN_EML_ARCHIVE_FIELD(KilnArchiveQuery, limit, 40);
KILN_EML_ARCHIVE_LAYOUT(KilnArchiveResourceInfo, 64, 8);
KILN_EML_ARCHIVE_FIELD(KilnArchiveResourceInfo, size, 0);
KILN_EML_ARCHIVE_FIELD(KilnArchiveResourceInfo, schema_version, 4);
KILN_EML_ARCHIVE_FIELD(KilnArchiveResourceInfo, key, 8);
KILN_EML_ARCHIVE_FIELD(KilnArchiveResourceInfo, source, 32);
KILN_EML_ARCHIVE_FIELD(KilnArchiveResourceInfo, reserved, 36);
KILN_EML_ARCHIVE_FIELD(KilnArchiveResourceInfo, type_index, 40);
KILN_EML_ARCHIVE_FIELD(KilnArchiveResourceInfo, byte_length, 48);
KILN_EML_ARCHIVE_FIELD(KilnArchiveResourceInfo, generation, 56);
KILN_EML_ARCHIVE_LAYOUT(KilnFieldSelection, 64, 8);
KILN_EML_ARCHIVE_FIELD(KilnFieldSelection, size, 0);
KILN_EML_ARCHIVE_FIELD(KilnFieldSelection, schema_version, 4);
KILN_EML_ARCHIVE_FIELD(KilnFieldSelection, offset, 8);
KILN_EML_ARCHIVE_FIELD(KilnFieldSelection, width, 16);
KILN_EML_ARCHIVE_FIELD(KilnFieldSelection, scalar_kind, 20);
KILN_EML_ARCHIVE_FIELD(KilnFieldSelection, type_index, 24);
KILN_EML_ARCHIVE_FIELD(KilnFieldSelection, root_type_index, 32);
KILN_EML_ARCHIVE_FIELD(KilnFieldSelection, registry_id, 40);
KILN_EML_ARCHIVE_FIELD(KilnFieldSelection, snapshot_length, 48);
KILN_EML_ARCHIVE_FIELD(KilnFieldSelection, flags, 56);
KILN_EML_ARCHIVE_FIELD(KilnFieldSelection, reserved, 60);
KILN_EML_ARCHIVE_LAYOUT(KilnArchiveValueRecordV1, 48, 8);
KILN_EML_ARCHIVE_FIELD(KilnArchiveValueRecordV1, size, 0);
KILN_EML_ARCHIVE_FIELD(KilnArchiveValueRecordV1, schema_version, 4);
KILN_EML_ARCHIVE_FIELD(KilnArchiveValueRecordV1, tag, 8);
KILN_EML_ARCHIVE_FIELD(KilnArchiveValueRecordV1, flags, 12);
KILN_EML_ARCHIVE_FIELD(KilnArchiveValueRecordV1, record_bytes, 16);
KILN_EML_ARCHIVE_FIELD(KilnArchiveValueRecordV1, payload_bytes, 24);
KILN_EML_ARCHIVE_FIELD(KilnArchiveValueRecordV1, child_count, 32);
KILN_EML_ARCHIVE_FIELD(KilnArchiveValueRecordV1, type_bytes, 40);
KILN_EML_ARCHIVE_FIELD(KilnArchiveValueRecordV1, reserved, 44);
KILN_EML_ARCHIVE_LAYOUT(KilnArchiveErrorV1, 32, 8);
KILN_EML_ARCHIVE_FIELD(KilnArchiveErrorV1, size, 0);
KILN_EML_ARCHIVE_FIELD(KilnArchiveErrorV1, result, 4);
KILN_EML_ARCHIVE_FIELD(KilnArchiveErrorV1, token, 8);
KILN_EML_ARCHIVE_FIELD(KilnArchiveErrorV1, message_bytes, 16);
KILN_EML_ARCHIVE_FIELD(KilnArchiveErrorV1, flags, 24);
KILN_EML_ARCHIVE_FIELD(KilnArchiveErrorV1, reserved, 28);
KILN_EML_ARCHIVE_LAYOUT(KilnEmlArchiveApiV1, 152, 8);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, header, 0);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, acquire, 8);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, identity, 16);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, resource_json, 24);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, release, 32);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, registry_metadata, 40);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, find_resource, 48);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, list_resources, 56);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, read_resource, 64);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, content_read_range, 72);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, list_contents, 80);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, value_decode, 88);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, value_encode, 96);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, loaded_field, 104);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, attribute_catalog, 112);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, attribute_resolve, 120);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, attribute_evaluate, 128);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, error_copy, 136);
KILN_EML_ARCHIVE_FIELD(KilnEmlArchiveApiV1, error_release, 144);
#endif
#undef KILN_EML_ARCHIVE_LAYOUT
#undef KILN_EML_ARCHIVE_FIELD
#undef KILN_EML_ARCHIVE_ALIGN
#undef KILN_EML_ARCHIVE_ASSERT
#endif
