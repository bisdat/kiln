/* SPDX-License-Identifier: MIT
 * Contract probes only. The placeholders below are NOT the GPL values provider,
 * and nothing here proves graph, GC, lifetime or Source parity (kiln-eml's own tests do). */
/* The canonical archive header comes FIRST: a second definition of any archive
 * record in the values header would fail this translation unit. */
#include "services/kiln_eml_archive.h"
#include "services/kiln_eml_values.h"
/* PROBE_LAYOUT_ONLY: the static layout and enum asserts alone, with no C library, for the
 * Windows x64 compile-only check in test-eml-values.sh. */
#ifndef PROBE_LAYOUT_ONLY
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#ifdef __cplusplus
#define PROBE_ASSERT static_assert
#define PROBE_ALIGNOF alignof
#else
#define PROBE_ASSERT _Static_assert
#define PROBE_ALIGNOF _Alignof
#endif
#define SIZE(t, n) PROBE_ASSERT(sizeof(t) == n, #t " size"); PROBE_ASSERT(PROBE_ALIGNOF(t) == 8, #t " align")
#define O(t, m, n) PROBE_ASSERT(offsetof(t, m) == n, #t "." #m)

SIZE(KilnEmlArchiveIdentityV1, 160);
SIZE(KilnEmlValueIdentityV1, 208);
SIZE(KilnEmlValueInfoV1, 248);
SIZE(KilnEmlValueSelectorV1, 32);
SIZE(KilnEmlValueResultV1, 48);
SIZE(KilnEmlValueOpenV1, 88);
SIZE(KilnEmlLuaAccessV1, 56);
SIZE(KilnEmlLuaValueV1, 56);
SIZE(KilnEmlLuaReaderV1, 56);
SIZE(KilnEmlValuesApiV1, 144);
O(KilnEmlValueIdentityV1, size, 0); O(KilnEmlValueIdentityV1, lua_phase, 4);
O(KilnEmlValueIdentityV1, creator_owner, 8); O(KilnEmlValueIdentityV1, arena, 16);
O(KilnEmlValueIdentityV1, node_id, 24); O(KilnEmlValueIdentityV1, revision, 32);
O(KilnEmlValueIdentityV1, layout_revision, 40); O(KilnEmlValueIdentityV1, archive, 48);
O(KilnEmlValueInfoV1, size, 0); O(KilnEmlValueInfoV1, requested, 4);
O(KilnEmlValueInfoV1, identity, 8); O(KilnEmlValueInfoV1, kind, 216);
O(KilnEmlValueInfoV1, flags, 220); O(KilnEmlValueInfoV1, type_index, 224);
O(KilnEmlValueInfoV1, qualified_hash, 228); O(KilnEmlValueInfoV1, logical_len, 232);
O(KilnEmlValueInfoV1, source_len, 240);
O(KilnEmlValueSelectorV1, size, 0); O(KilnEmlValueSelectorV1, kind, 4);
O(KilnEmlValueSelectorV1, index, 8); O(KilnEmlValueSelectorV1, field, 16);
O(KilnEmlValueResultV1, size, 0); O(KilnEmlValueResultV1, disposition, 4);
O(KilnEmlValueResultV1, flags, 8); O(KilnEmlValueResultV1, arity, 12);
O(KilnEmlValueResultV1, item, 16); O(KilnEmlValueResultV1, payload, 24);
O(KilnEmlValueResultV1, error, 32); O(KilnEmlValueResultV1, receiver_revision, 40);
O(KilnEmlValueOpenV1, size, 0); O(KilnEmlValueOpenV1, origin, 4);
O(KilnEmlValueOpenV1, qualified_type, 8); O(KilnEmlValueOpenV1, key, 24);
O(KilnEmlValueOpenV1, source_instance, 48); O(KilnEmlValueOpenV1, source_session, 56);
O(KilnEmlValueOpenV1, bytes, 64); O(KilnEmlValueOpenV1, length, 72);
O(KilnEmlValueOpenV1, lua_object, 80);
O(KilnEmlLuaAccessV1, size, 0); O(KilnEmlLuaAccessV1, operation, 4);
O(KilnEmlLuaAccessV1, object, 8); O(KilnEmlLuaAccessV1, index, 16);
O(KilnEmlLuaAccessV1, key, 24); O(KilnEmlLuaAccessV1, arena, 40);
O(KilnEmlLuaAccessV1, node_id, 48);
O(KilnEmlLuaValueV1, size, 0); O(KilnEmlLuaValueV1, kind, 4);
O(KilnEmlLuaValueV1, object, 8); O(KilnEmlLuaValueV1, bits, 16);
O(KilnEmlLuaValueV1, byte_length, 24); O(KilnEmlLuaValueV1, typed_value, 32);
O(KilnEmlLuaValueV1, error_object, 40); O(KilnEmlLuaValueV1, adapter_class, 48);
O(KilnEmlLuaValueV1, lease_disposition, 52);
O(KilnEmlLuaReaderV1, size, 0); O(KilnEmlLuaReaderV1, version, 4);
O(KilnEmlLuaReaderV1, user, 8); O(KilnEmlLuaReaderV1, access, 16);
O(KilnEmlLuaReaderV1, copy, 24); O(KilnEmlLuaReaderV1, retain, 32);
O(KilnEmlLuaReaderV1, release, 40); O(KilnEmlLuaReaderV1, wake, 48);
O(KilnEmlValuesApiV1, header, 0); O(KilnEmlValuesApiV1, arena_create, 8);
O(KilnEmlValuesApiV1, arena_close, 16); O(KilnEmlValuesApiV1, arena_collect, 24);
O(KilnEmlValuesApiV1, value_open, 32); O(KilnEmlValuesApiV1, value_info, 40);
O(KilnEmlValuesApiV1, value_read, 48); O(KilnEmlValuesApiV1, value_child, 56);
O(KilnEmlValuesApiV1, value_set, 64); O(KilnEmlValuesApiV1, value_clone, 72);
O(KilnEmlValuesApiV1, value_export, 80); O(KilnEmlValuesApiV1, value_close, 88);
O(KilnEmlValuesApiV1, value_result_copy, 96); O(KilnEmlValuesApiV1, value_array_push, 104);
O(KilnEmlValuesApiV1, value_array_insert, 112); O(KilnEmlValuesApiV1, value_array_pop, 120);
O(KilnEmlValuesApiV1, value_array_remove, 128); O(KilnEmlValuesApiV1, value_array_clear, 136);
/* Widths the offsets alone would not catch at the record tail. */
PROBE_ASSERT(sizeof(((KilnEmlValueOpenV1*)0)->length) == 8, "open length is size_t on x64");
PROBE_ASSERT(sizeof(((KilnEmlLuaAccessV1*)0)->index) == 8, "access index is 64-bit");
PROBE_ASSERT(sizeof(((KilnEmlLuaValueV1*)0)->lease_disposition) == 4, "lease disposition is u32");

/* Every enum value is ABI: the GPL provider and the MIT frontend exchange these numbers. */
PROBE_ASSERT(KILN_EML_VALUES_VERSION == 1u, "version");
PROBE_ASSERT(KILN_VALUE_STARTUP == 0 && KILN_VALUE_RUNTIME == 1, "phases");
PROBE_ASSERT(KILN_VALUE_NIL == 1 && KILN_VALUE_BOOLEAN == 2 && KILN_VALUE_INTEGER == 3 &&
             KILN_VALUE_NUMBER == 4 && KILN_VALUE_STRING == 5, "scalar kinds");
PROBE_ASSERT(KILN_VALUE_STRUCT_OWNED == 16 && KILN_VALUE_STRUCT_MAPPED == 17 &&
             KILN_VALUE_ARRAY_OWNED == 18 && KILN_VALUE_ARRAY_MAPPED == 19 &&
             KILN_VALUE_VARIANT_OWNED == 20 && KILN_VALUE_VARIANT_MAPPED == 21 &&
             KILN_VALUE_ITERATOR == 22 && KILN_VALUE_RESULT == 23 && KILN_VALUE_ERROR == 24, "node kinds");
PROBE_ASSERT(KILN_VALUE_FIELD == 1 && KILN_VALUE_INDEX == 2, "selector kinds");
PROBE_ASSERT(KILN_VALUE_HANDLED == 1 && KILN_VALUE_UNSUPPORTED == 2 && KILN_VALUE_SEMANTIC_ERROR == 3, "dispositions");
PROBE_ASSERT(KILN_VALUE_MUTATED == 1, "result flags");
PROBE_ASSERT(KILN_VALUE_INFO_LENGTH == 1 && KILN_VALUE_INFO_DIRTY == 2 && KILN_VALUE_INFO_SOURCE_LENGTH == 4, "info requests");
PROBE_ASSERT(KILN_VALUE_STATIC_ARRAY == 1 && KILN_VALUE_DIRTY == 2 &&
             KILN_VALUE_RESOURCE_ROOT == 4 && KILN_VALUE_LOADED_SNAPSHOT == 8, "info flags");
PROBE_ASSERT(KILN_VALUE_READ_VIEW == 0 && KILN_VALUE_READ_LENGTH == 1 && KILN_VALUE_READ_DIRTY == 2 &&
             KILN_VALUE_READ_STRINGIFY == 3 && KILN_VALUE_READ_SOURCE_BYTES == 4 &&
             KILN_VALUE_READ_ITER_BEGIN == 5 && KILN_VALUE_READ_ITER_NEXT == 6, "read queries");
PROBE_ASSERT(KILN_VALUE_OPEN_RESOURCE == 1 && KILN_VALUE_OPEN_BYTES == 2 && KILN_VALUE_OPEN_LUA_INPUT == 3 &&
             KILN_VALUE_OPEN_LOADED_SNAPSHOT == 4 && KILN_VALUE_OPEN_DEFAULT == 5, "open origins");
PROBE_ASSERT(KILN_LUA_DESCRIBE == 0 && KILN_LUA_GET_FIELD_VALUE == 1 && KILN_LUA_GET_FIELD_STRING == 2 &&
             KILN_LUA_LENGTH == 3 && KILN_LUA_RAW_SEQUENCE_INDEX == 4 && KILN_LUA_SOURCE_TO_STRING == 5 &&
             KILN_LUA_MAKE_TYPED_PROXY == 6, "reader operations");
PROBE_ASSERT(KILN_LUA_NIL == 0 && KILN_LUA_BOOLEAN == 1 && KILN_LUA_INTEGER == 2 && KILN_LUA_NUMBER == 3 &&
             KILN_LUA_STRING == 4 && KILN_LUA_TABLE == 5 && KILN_LUA_TYPED_VALUE == 6 &&
             KILN_LUA_KNOWN_USERDATA == 7 && KILN_LUA_OTHER_USERDATA == 8 && KILN_LUA_OTHER == 9, "Lua kinds");
PROBE_ASSERT(KILN_LUA_ADAPTER_NONE == 0 && KILN_LUA_ADAPTER_GUID == 1 && KILN_LUA_ADAPTER_CONTENT == 2 &&
             KILN_LUA_ADAPTER_RESOURCE == 3, "adapter classes");
PROBE_ASSERT(KILN_LUA_LEASE_NONE == 0 && KILN_LUA_LEASE_ADOPTED == 1 && KILN_LUA_LEASE_REUSED == 2, "lease dispositions");

#ifndef PROBE_LAYOUT_ONLY
/* The header's rule, as a probe-local helper: check extent, version and non-null
 * before EVERY reader callback. The size test comes first, so a truncated table's
 * version or tail is never read past its declared extent. The {size, version} prefix
 * is read as raw words: a member access through the full struct type on a shorter
 * allocation is itself undefined (UBSan object-size reports it). */
static uint32_t reader_word(const void* r, size_t i) {
    uint32_t w;
    memcpy(&w, (const unsigned char*)r + i * sizeof w, sizeof w);
    return w;
}
#define READER_HAS(r, m) \
    ((r) != NULL && reader_word((r), 0) >= offsetof(KilnEmlLuaReaderV1, m) + sizeof((r)->m) && \
     reader_word((r), 1) == 1 && (r)->m != NULL)
/* Table members: the usual Kiln extent/version/non-null rule (no macro in the header). */
#define VALUES_HAS(api, m) \
    ((api) != NULL && (api)->header.size >= offsetof(KilnEmlValuesApiV1, m) + sizeof((api)->m) && \
     (api)->header.version >= KILN_EML_VALUES_VERSION && (api)->m != NULL)

static int reader_calls;
static KilnResult KILN_CALL r_access(void* u, const KilnEmlLuaAccessV1* a, KilnEmlLuaValueV1* v) {
    (void)u; (void)a; (void)v; ++reader_calls; return KILN_OK;
}
static KilnResult KILN_CALL r_copy(void* u, uint64_t o, void* out, size_t cap, size_t* n) {
    (void)u; (void)o; (void)out; (void)cap; (void)n; ++reader_calls; return KILN_OK;
}
static KilnResult KILN_CALL r_retain(void* u, uint64_t o) { (void)u; (void)o; ++reader_calls; return KILN_OK; }
static void KILN_CALL r_release(void* u, uint64_t o) { (void)u; (void)o; ++reader_calls; }
static void KILN_CALL r_wake(void* u, KilnHandle a) { (void)u; (void)a; ++reader_calls; }

/* Placeholders with the exact member types: assigning them pins every signature
 * (a changed parameter or return type fails to compile in C++). Only value_close is called. */
static KilnResult KILN_CALL arena_create(KilnOwner o, KilnEmlArchiveView v, uint32_t phase,
                                        const KilnEmlLuaReaderV1* r, KilnHandle* arena) {
    (void)o; (void)v; (void)phase;
    if (!arena || !READER_HAS(r, access) || !READER_HAS(r, release)) return KILN_ERR_INVALID_ARGUMENT;
    *arena = 7; return KILN_OK;
}
static KilnResult KILN_CALL arena_close(KilnOwner o, KilnHandle a, uint32_t* pending) {
    (void)o; (void)a; (void)pending; return KILN_OK;
}
static KilnResult KILN_CALL value_open(KilnOwner o, KilnHandle a, KilnEmlArchiveView v,
                                      const KilnEmlValueOpenV1* in, KilnEmlValueResultV1* out) {
    (void)o; (void)a; (void)v; (void)in; (void)out; return KILN_OK;
}
static KilnResult KILN_CALL value_info(KilnOwner o, KilnHandle h, KilnEmlValueInfoV1* out) {
    (void)o; (void)h; (void)out; return KILN_OK;
}
static KilnResult KILN_CALL value_read(KilnOwner o, KilnHandle h, uint32_t q, KilnEmlValueResultV1* out) {
    (void)o; (void)h; (void)q; (void)out; return KILN_OK;
}
static KilnResult KILN_CALL value_child(KilnOwner o, KilnHandle h, const KilnEmlValueSelectorV1* s,
                                       KilnEmlValueResultV1* out) {
    (void)o; (void)h; (void)s; (void)out; return KILN_OK;
}
static KilnResult KILN_CALL value_set(KilnOwner o, KilnHandle h, const KilnEmlValueSelectorV1* s,
                                     uint64_t lua, KilnEmlValueResultV1* out) {
    (void)o; (void)h; (void)s; (void)lua; (void)out; return KILN_OK;
}
static KilnResult KILN_CALL value_unary(KilnOwner o, KilnHandle h, KilnEmlValueResultV1* out) {
    (void)o; (void)h; (void)out; return KILN_OK;
}
static KilnResult KILN_CALL value_export(KilnOwner o, KilnHandle h, KilnStr t, KilnEmlValueResultV1* out) {
    (void)o; (void)h; (void)t; (void)out; return KILN_OK;
}
/* Issued tokens close idempotently; a never-issued token is NOT_FOUND (header rule). */
static KilnResult KILN_CALL value_close(KilnOwner o, KilnHandle h) {
    (void)o; return h == 42 ? KILN_OK : KILN_ERR_NOT_FOUND;
}
static KilnResult KILN_CALL value_result_copy(KilnOwner o, KilnHandle h, void* out, size_t cap, size_t* n) {
    (void)o; (void)h; (void)out; (void)cap; (void)n; return KILN_OK;
}
static KilnResult KILN_CALL value_array_push(KilnOwner o, KilnHandle h, uint64_t lua, KilnEmlValueResultV1* out) {
    (void)o; (void)h; (void)lua; (void)out; return KILN_OK;
}
static KilnResult KILN_CALL value_array_insert(KilnOwner o, KilnHandle h, uint64_t i, uint64_t lua,
                                              KilnEmlValueResultV1* out) {
    (void)o; (void)h; (void)i; (void)lua; (void)out; return KILN_OK;
}
static KilnResult KILN_CALL value_array_remove(KilnOwner o, KilnHandle h, uint64_t i, KilnEmlValueResultV1* out) {
    (void)o; (void)h; (void)i; (void)out; return KILN_OK;
}

static void fill(KilnEmlValuesApiV1* api) {
    memset(api, 0, sizeof *api);
    api->header.size = sizeof *api; api->header.version = KILN_EML_VALUES_VERSION;
    api->arena_create = arena_create; api->arena_close = arena_close; api->arena_collect = arena_close;
    api->value_open = value_open; api->value_info = value_info; api->value_read = value_read;
    api->value_child = value_child; api->value_set = value_set; api->value_clone = value_unary;
    api->value_export = value_export; api->value_close = value_close;
    api->value_result_copy = value_result_copy; api->value_array_push = value_array_push;
    api->value_array_insert = value_array_insert; api->value_array_pop = value_unary;
    api->value_array_remove = value_array_remove; api->value_array_clear = value_unary;
}

int main(void) {
    KilnEmlValuesApiV1 api;
    KilnEmlLuaReaderV1 reader;
    KilnHandle arena = 0;
    assert(strcmp(KILN_EML_VALUES_SERVICE, "kiln.eml.values") == 0);
    assert(strcmp(KILN_EML_VALUES_SERVICE, KILN_EML_ARCHIVE_SERVICE) != 0);

    /* Table extents: 17 members, the last one ends at 144. */
    fill(&api);
    assert(VALUES_HAS(&api, arena_create) && VALUES_HAS(&api, value_array_clear));
    api.header.size = 143;
    assert(!VALUES_HAS(&api, value_array_clear) && VALUES_HAS(&api, value_array_remove));
    api.header.size = 15;
    assert(!VALUES_HAS(&api, arena_create));
    api.header.size = sizeof api; api.header.version = 0;
    assert(!VALUES_HAS(&api, arena_create));
    api.header.version = 1; api.value_close = NULL;
    assert(!VALUES_HAS(&api, value_close));
    fill(&api);
    assert(api.value_close(1, 42) == KILN_OK && api.value_close(1, 42) == KILN_OK);
    assert(api.value_close(1, 0) == KILN_ERR_NOT_FOUND && api.value_close(1, 43) == KILN_ERR_NOT_FOUND);

    /* Reader: full table admits every callback. */
    memset(&reader, 0, sizeof reader);
    reader.size = sizeof reader; reader.version = 1;
    reader.access = r_access; reader.copy = r_copy; reader.retain = r_retain;
    reader.release = r_release; reader.wake = r_wake;
    assert(READER_HAS(&reader, access) && READER_HAS(&reader, copy) && READER_HAS(&reader, retain) &&
           READER_HAS(&reader, release) && READER_HAS(&reader, wake));
    assert(api.arena_create(1, 9, KILN_VALUE_STARTUP, &reader, &arena) == KILN_OK && arena == 7);
    /* Each truncation hides exactly the members past it, never more, never fewer. */
    reader.size = (uint32_t)offsetof(KilnEmlLuaReaderV1, wake);
    assert(!READER_HAS(&reader, wake) && READER_HAS(&reader, release));
    reader.size = (uint32_t)offsetof(KilnEmlLuaReaderV1, wake) + 7;
    assert(!READER_HAS(&reader, wake));
    reader.size = (uint32_t)offsetof(KilnEmlLuaReaderV1, release);
    assert(!READER_HAS(&reader, release) && READER_HAS(&reader, retain));
    reader.size = (uint32_t)offsetof(KilnEmlLuaReaderV1, access);
    assert(!READER_HAS(&reader, access));
    arena = 0;
    assert(api.arena_create(1, 9, KILN_VALUE_RUNTIME, &reader, &arena) == KILN_ERR_INVALID_ARGUMENT && arena == 0);
    /* Wrong version or a null member refuses even at full size. */
    reader.size = sizeof reader; reader.version = 2;
    assert(!READER_HAS(&reader, access));
    reader.version = 1; reader.release = NULL;
    assert(!READER_HAS(&reader, release));
    assert(api.arena_create(1, 9, KILN_VALUE_RUNTIME, &reader, &arena) == KILN_ERR_INVALID_ARGUMENT && arena == 0);
    assert(!READER_HAS((const KilnEmlLuaReaderV1*)NULL, access));
    /* An actually short allocation: only {size, version}. ASan reports any read past it. */
    void* raw = malloc(2 * sizeof(uint32_t));
    assert(raw);
    ((uint32_t*)raw)[0] = 2 * sizeof(uint32_t); ((uint32_t*)raw)[1] = 1;
    const KilnEmlLuaReaderV1* tiny = (const KilnEmlLuaReaderV1*)raw;
    assert(!READER_HAS(tiny, access) && !READER_HAS(tiny, wake));
    free(raw);
    assert(reader_calls == 0); /* structural checks never invoke a callback */

    /* Records are caller-sized: v1 sizes are the published minimums. */
    KilnEmlValueResultV1 result;
    memset(&result, 0, sizeof result);
    result.size = sizeof result; result.disposition = KILN_VALUE_HANDLED; result.flags = KILN_VALUE_MUTATED;
    assert(result.size == 48 && result.item == 0);
    puts("eml values: contract probes passed (placeholders, not the provider)");
    return 0;
}
#endif /* PROBE_LAYOUT_ONLY */
