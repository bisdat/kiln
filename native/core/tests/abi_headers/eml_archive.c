/* SPDX-License-Identifier: MIT
 * Contract probes only. The fake below is NOT the GPL archive provider. */
#include "services/kiln_eml_archive.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ArchiveOffsetPrefix {
    KilnServiceHeader header;
    KilnResult(KILN_CALL* acquire)(KilnOwner, const KilnEmlArchiveIdentityV1*, KilnEmlArchiveView*);
    KilnResult(KILN_CALL* identity)(KilnEmlArchiveView, KilnEmlArchiveIdentityV1*);
    KilnResult(KILN_CALL* resource_json)(KilnEmlArchiveView, KilnStr, char*, size_t, size_t*, KilnArchiveErrorV1*);
    KilnResult(KILN_CALL* release)(KilnEmlArchiveView);
} ArchiveOffsetPrefix;

static KilnResult KILN_CALL acquire(KilnOwner owner, const KilnEmlArchiveIdentityV1* expected,
                                   KilnEmlArchiveView* out) {
    if (!expected || expected->size < sizeof *expected || !out) return KILN_ERR_INVALID_ARGUMENT;
    if (owner == 99) return KILN_ERR_REVOKED;
    if (expected->phase != KILN_EML_ARCHIVE_PREPARED && expected->phase != KILN_EML_ARCHIVE_PUBLISHED)
        return KILN_ERR_INVALID_ARGUMENT;
    if (expected->phase == KILN_EML_ARCHIVE_PUBLISHED) return KILN_ERR_UNAVAILABLE; /* fake has only a prepared grant */
    *out = 42;
    return KILN_OK;
}
static KilnResult KILN_CALL identity(KilnEmlArchiveView view, KilnEmlArchiveIdentityV1* out) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    return view == 42 ? KILN_OK : KILN_ERR_REVOKED;
}
static KilnResult KILN_CALL release(KilnEmlArchiveView view) {
    return view == 42 ? KILN_OK : KILN_ERR_INVALID_ARGUMENT;
}
static KilnResult KILN_CALL metadata(KilnEmlArchiveView view, uint64_t first, uint32_t limit,
                                    char* out, size_t cap, size_t* required, KilnArchiveErrorV1* error) {
    (void)error;
    static const char reply[] = "{\"schema_version\":1}";
    if (!required || (!out && cap) || UINT64_MAX - first < limit) return KILN_ERR_INVALID_ARGUMENT;
    if (view != 42) return KILN_ERR_REVOKED;
    *required = sizeof reply - 1;
    if (cap < *required) return KILN_ERR_BUFFER_TOO_SMALL;
    memcpy(out, reply, *required);
    return KILN_OK;
}
static int registry_consumer(const KilnEmlArchiveApiV1* api, int metadata_row, int member_rows) {
    return metadata_row && member_rows && KILN_EML_ARCHIVE_HAS(api, acquire) && KILN_EML_ARCHIVE_HAS(api, identity) &&
           KILN_EML_ARCHIVE_HAS(api, release) && KILN_EML_ARCHIVE_HAS(api, registry_metadata);
}
/* Non-null placeholders for structural tests only; none of these is called. */
static KilnResult KILN_CALL resource_json(KilnEmlArchiveView v, KilnStr q, char* out, size_t cap, size_t* n, KilnArchiveErrorV1* error) {
    (void)error;
    (void)v; (void)q; (void)out; (void)cap; (void)n; return KILN_OK;
}
static KilnResult KILN_CALL find_resource(KilnOwner o, KilnEmlArchiveView v, const KilnAssetKey* k,
                                         KilnArchiveResourceInfo* out, KilnArchiveErrorV1* error) {
    (void)error;
    (void)o; (void)v; (void)k; (void)out; return KILN_OK;
}
static KilnResult KILN_CALL list_resources(KilnOwner o, KilnEmlArchiveView v, const KilnArchiveQuery* q,
                                          uint64_t first, KilnAssetKey* out, size_t cap, size_t* n, KilnArchiveErrorV1* error) {
    (void)error;
    (void)o; (void)v; (void)q; (void)first; (void)out; (void)cap; (void)n; return KILN_OK;
}
static KilnResult KILN_CALL read_resource(KilnOwner o, KilnEmlArchiveView v, const KilnAssetKey* k,
                                         void* out, size_t cap, size_t* n, KilnArchiveErrorV1* error) {
    (void)error;
    (void)o; (void)v; (void)k; (void)out; (void)cap; (void)n; return KILN_OK;
}
static KilnResult KILN_CALL read_content(KilnOwner o, KilnEmlArchiveView v, const KilnContentHash* h,
                                        uint64_t first, uint64_t count, void* out, size_t cap, size_t* n, KilnArchiveErrorV1* error) {
    (void)error;
    (void)o; (void)v; (void)h; (void)first; (void)count; (void)out; (void)cap; (void)n; return KILN_OK;
}
static KilnResult KILN_CALL list_contents(KilnOwner o, KilnEmlArchiveView v, uint64_t first,
                                         KilnContentHash* out, size_t cap, size_t* n, KilnArchiveErrorV1* error) {
    (void)error;
    (void)o; (void)v; (void)first; (void)out; (void)cap; (void)n; return KILN_OK;
}
static KilnResult KILN_CALL codec(KilnOwner o, KilnEmlArchiveView v, KilnStr t, const void* in,
                                 size_t len, void* out, size_t cap, size_t* n, KilnArchiveErrorV1* error) {
    (void)error;
    (void)o; (void)v; (void)t; (void)in; (void)len; (void)out; (void)cap; (void)n; return KILN_OK;
}
static KilnResult KILN_CALL loaded_field(KilnOwner o, KilnEmlArchiveView v, KilnStr t, KilnStr p,
                                        const void* in, size_t len, KilnFieldSelection* out, KilnArchiveErrorV1* error) {
    (void)error;
    (void)o; (void)v; (void)t; (void)p; (void)in; (void)len; (void)out; return KILN_OK;
}
static KilnResult KILN_CALL attribute_catalog(KilnOwner o, KilnEmlArchiveView v, char* out, size_t cap, size_t* n, KilnArchiveErrorV1* error) {
    (void)error;
    (void)o; (void)v; (void)out; (void)cap; (void)n; return KILN_OK;
}
static KilnResult KILN_CALL attribute_resolve(KilnOwner o, KilnEmlArchiveView v, KilnStr s,
                                             char* out, size_t cap, size_t* n, KilnArchiveErrorV1* error) {
    (void)error;
    (void)o; (void)v; (void)s; (void)out; (void)cap; (void)n; return KILN_OK;
}
static KilnResult KILN_CALL attribute_evaluate(KilnOwner o, KilnEmlArchiveView v, uint32_t root,
                                              const uint32_t* in, uint32_t count, uint32_t* out,
                                              char* trace, size_t cap, size_t* n, KilnArchiveErrorV1* error) {
    (void)error;
    (void)o; (void)v; (void)root; (void)in; (void)count; (void)out; (void)trace; (void)cap; (void)n; return KILN_OK;
}
static KilnResult KILN_CALL copy_error(KilnOwner o, KilnHandle h, void* out, size_t cap, size_t* n) {
    (void)o; (void)h; (void)out; (void)cap; (void)n; return KILN_ERR_UNAVAILABLE;
}
static KilnResult KILN_CALL close_error(KilnOwner o, KilnHandle h) {
    (void)o; (void)h; return KILN_ERR_UNAVAILABLE;
}
/* Fake provider support facts. Real providers must also prove useful success and
 * generation readiness; non-null slots alone do not establish those facts. */
static int group_available(const KilnEmlArchiveApiV1* api, const char* row) {
    if (strcmp(row, KILN_EML_ARCHIVE_METADATA_CAP) == 0)
        return KILN_EML_ARCHIVE_HAS(api, acquire) && KILN_EML_ARCHIVE_HAS(api, identity) &&
               KILN_EML_ARCHIVE_HAS(api, release) && KILN_EML_ARCHIVE_HAS(api, registry_metadata);
    if (strcmp(row, KILN_EML_ARCHIVE_READ_CAP) == 0)
        return KILN_EML_ARCHIVE_HAS(api, resource_json) && KILN_EML_ARCHIVE_HAS(api, find_resource) &&
               KILN_EML_ARCHIVE_HAS(api, list_resources) && KILN_EML_ARCHIVE_HAS(api, read_resource) &&
               KILN_EML_ARCHIVE_HAS(api, content_read_range) && KILN_EML_ARCHIVE_HAS(api, list_contents);
    if (strcmp(row, KILN_EML_ARCHIVE_CODEC_CAP) == 0)
        return KILN_EML_ARCHIVE_HAS(api, value_decode) && KILN_EML_ARCHIVE_HAS(api, value_encode) &&
               KILN_EML_ARCHIVE_HAS(api, loaded_field);
    if (strcmp(row, KILN_EML_ARCHIVE_ERRORS_CAP) == 0)
        return KILN_EML_ARCHIVE_HAS(api, error_copy) && KILN_EML_ARCHIVE_HAS(api, error_release);
    if (strcmp(row, KILN_EML_ARCHIVE_ATTRIBUTES_CAP) == 0)
        return KILN_EML_ARCHIVE_HAS(api, attribute_catalog) && KILN_EML_ARCHIVE_HAS(api, attribute_resolve) &&
               KILN_EML_ARCHIVE_HAS(api, attribute_evaluate);
    return 0;
}
static void capability_groups(void) {
    KilnEmlArchiveApiV1 api;
    memset(&api, 0, sizeof api); api.header.size = 48; api.header.version = 1;
    api.acquire = acquire; api.identity = identity; api.release = release; api.registry_metadata = metadata;
    assert(strcmp(KILN_EML_ARCHIVE_METADATA_CAP, "eml.archive.metadata") == 0);
    assert(group_available(&api, KILN_EML_ARCHIVE_METADATA_CAP));
    assert(registry_consumer(&api, group_available(&api, KILN_EML_ARCHIVE_METADATA_CAP), 1));
    assert(!group_available(&api, KILN_EML_ARCHIVE_READ_CAP));
    api.header.size = sizeof api;
    api.resource_json = resource_json; api.find_resource = find_resource; api.list_resources = list_resources;
    api.read_resource = read_resource; api.content_read_range = read_content; api.list_contents = list_contents;
    api.value_decode = codec; api.value_encode = codec; api.loaded_field = loaded_field;
    api.attribute_catalog = attribute_catalog; api.attribute_resolve = attribute_resolve;
    api.attribute_evaluate = attribute_evaluate;
    api.error_copy = copy_error; api.error_release = close_error;
    const char* rows[] = {KILN_EML_ARCHIVE_METADATA_CAP, KILN_EML_ARCHIVE_READ_CAP,
                          KILN_EML_ARCHIVE_CODEC_CAP, KILN_EML_ARCHIVE_ATTRIBUTES_CAP, KILN_EML_ARCHIVE_ERRORS_CAP};
    for (size_t i = 0; i < sizeof rows / sizeof *rows; ++i) assert(group_available(&api, rows[i]));
    /* Any missing member makes its WHOLE group unavailable, not a partial row. */
#define INCOMPLETE(member, row) do { \
    KilnEmlArchiveApiV1 partial = api; partial.member = NULL; \
    assert(!group_available(&partial, row)); \
} while (0)
    INCOMPLETE(acquire, KILN_EML_ARCHIVE_METADATA_CAP);
    INCOMPLETE(identity, KILN_EML_ARCHIVE_METADATA_CAP);
    INCOMPLETE(release, KILN_EML_ARCHIVE_METADATA_CAP);
    INCOMPLETE(registry_metadata, KILN_EML_ARCHIVE_METADATA_CAP);
    INCOMPLETE(resource_json, KILN_EML_ARCHIVE_READ_CAP);
    INCOMPLETE(find_resource, KILN_EML_ARCHIVE_READ_CAP);
    INCOMPLETE(list_resources, KILN_EML_ARCHIVE_READ_CAP);
    INCOMPLETE(read_resource, KILN_EML_ARCHIVE_READ_CAP);
    INCOMPLETE(content_read_range, KILN_EML_ARCHIVE_READ_CAP);
    INCOMPLETE(list_contents, KILN_EML_ARCHIVE_READ_CAP);
    INCOMPLETE(value_decode, KILN_EML_ARCHIVE_CODEC_CAP);
    INCOMPLETE(value_encode, KILN_EML_ARCHIVE_CODEC_CAP);
    INCOMPLETE(loaded_field, KILN_EML_ARCHIVE_CODEC_CAP);
    INCOMPLETE(attribute_catalog, KILN_EML_ARCHIVE_ATTRIBUTES_CAP);
    INCOMPLETE(attribute_resolve, KILN_EML_ARCHIVE_ATTRIBUTES_CAP);
    INCOMPLETE(attribute_evaluate, KILN_EML_ARCHIVE_ATTRIBUTES_CAP);
    INCOMPLETE(error_copy, KILN_EML_ARCHIVE_ERRORS_CAP);
    INCOMPLETE(error_release, KILN_EML_ARCHIVE_ERRORS_CAP);
#undef INCOMPLETE
    api.header.size = 87;
    assert(!group_available(&api, KILN_EML_ARCHIVE_READ_CAP));
    api.header.size = 111;
    assert(!group_available(&api, KILN_EML_ARCHIVE_CODEC_CAP));
    api.header.size = 135;
    assert(!group_available(&api, KILN_EML_ARCHIVE_ATTRIBUTES_CAP));
    api.header.size = 151;
    assert(!group_available(&api, KILN_EML_ARCHIVE_ERRORS_CAP));
}

/* Test-only validators pin down packet arithmetic; the provider's own tests test its real bodies. */
static KilnResult query_record(const KilnArchiveQuery* q, uint64_t first) {
    if (!q || q->size < sizeof *q || q->schema_version != 1 || (q->flags & ~7u) || q->order > 1)
        return KILN_ERR_INVALID_ARGUMENT;
    if (q->order == KILN_ARCHIVE_ORDER_PART && (q->flags & 3u) != 3u) return KILN_ERR_INVALID_ARGUMENT;
    if (q->limit && UINT64_MAX - first < q->limit) return KILN_ERR_INVALID_ARGUMENT;
    return KILN_OK;
}
static KilnResult content_range(uint64_t start, uint64_t count, void* out, size_t cap, size_t* required) {
    static const unsigned char content[] = {0, 0xff, 0, 1};
    if (!required || (!out && cap) || start > sizeof content || count > sizeof content - start)
        return KILN_ERR_INVALID_ARGUMENT;
    *required = (size_t)count;
    if (cap < count) return KILN_ERR_BUFFER_TOO_SMALL;
    if (count) memcpy(out, content + start, (size_t)count);
    return KILN_OK;
}
static KilnEmlArchiveApiV1 table;
/* Both discovery paths in the fake deliberately return one address. The real export and registration are the provider's. */
static const KilnEmlArchiveApiV1* KILN_CALL early(uint32_t version) { return version <= 1 ? &table : NULL; }
static const void* lookup(void) { return &table; }

int main(void) {
    KilnEmlArchiveIdentityV1 expected;
    KilnEmlArchiveView view = 0;
    char data[64];
    size_t required = 0;
    ArchiveOffsetPrefix prefix;
    memset(&table, 0, sizeof table);
    table.header.size = sizeof table;
    table.header.version = 1;
    table.acquire = acquire; table.identity = identity; table.release = release;
    table.registry_metadata = metadata;
    assert(sizeof prefix == 40 && sizeof table == 152);
    assert(sizeof expected.launch_id == 16 && sizeof expected.plan_id == 16);
    /* Query limit width: offset 40 and size 48 still hold with a 32-bit limit. */
    assert(sizeof(((KilnArchiveQuery*)0)->limit) == sizeof(uint64_t));
    assert(!KILN_EML_ARCHIVE_HAS(NULL, acquire));
    /* Even header.version is unavailable in an allocation declaring only size. */
    void* short_header = malloc(sizeof(uint32_t));
    assert(short_header);
    *(uint32_t*)short_header = sizeof(uint32_t);
    assert(!KILN_EML_ARCHIVE_HAS(short_header, acquire));
    free(short_header);
    assert(registry_consumer(&table, 1, 1)); /* resource_json and every later member may be null. */
    assert(!registry_consumer(&table, 0, 1)); /* held/off/absent metadata row */
    assert(!registry_consumer(&table, 1, 0)); /* absent required member row */
    capability_groups();
    table.header.size = 47;
    assert(!registry_consumer(&table, 1, 1));
    table.header.size = 48;
    assert(registry_consumer(&table, 1, 1));
    table.registry_metadata = NULL;
    assert(!registry_consumer(&table, 1, 1));
    table.registry_metadata = metadata;
    table.header.version = 0;
    assert(!registry_consumer(&table, 1, 1));
    table.header.version = 1; table.header.size = sizeof table;
    memcpy(&prefix, &table, sizeof prefix);
    assert(prefix.acquire == acquire && prefix.identity == identity && prefix.release == release);
    assert(prefix.resource_json == NULL);
    KilnEmlArchiveServiceFn getter = early;
    assert(getter(0) == lookup() && getter(1) == lookup() && getter(2) == NULL);
    memset(&expected, 0, sizeof expected);
    expected.size = sizeof expected - 1;
    assert(acquire(1, &expected, &view) == KILN_ERR_INVALID_ARGUMENT && view == 0);
    expected.size = sizeof expected;
    assert(acquire(1, &expected, &view) == KILN_ERR_INVALID_ARGUMENT && view == 0);
    expected.phase = KILN_EML_ARCHIVE_PUBLISHED;
    assert(acquire(1, &expected, &view) == KILN_ERR_UNAVAILABLE && view == 0);
    expected.phase = KILN_EML_ARCHIVE_PREPARED;
    assert(acquire(99, &expected, &view) == KILN_ERR_REVOKED && view == 0);
    assert(acquire(1, &expected, &view) == KILN_OK && view == 42);
    memset(data, 0x5a, sizeof data);
    assert(metadata(view, 0, 0, data, 1, &required, NULL) == KILN_ERR_BUFFER_TOO_SMALL);
    for (size_t i = 0; i < sizeof data; ++i) assert(data[i] == 0x5a);
    assert(metadata(view, 0, 0, data, required, &required, NULL) == KILN_OK);
    assert(data[required] == 0x5a); /* counted JSON, no terminator */
    assert(metadata(view, UINT64_MAX, 1, data, sizeof data, &required, NULL) == KILN_ERR_INVALID_ARGUMENT);
    assert(release(view) == KILN_OK && release(view) == KILN_OK && release(777) == KILN_ERR_INVALID_ARGUMENT);
    /* An actually older allocation, not just a full table advertising a short size. */
    prefix.header.size = sizeof prefix;
    assert(!KILN_EML_ARCHIVE_HAS(&prefix, registry_metadata));
    assert(KILN_EML_ARCHIVE_HAS(&prefix, release));
    KilnArchiveQuery q;
    memset(&q, 0, sizeof q); q.size = sizeof q; q.schema_version = 1;
    assert(query_record(&q, 0) == KILN_OK);
    q.size--;
    assert(query_record(&q, 0) == KILN_ERR_INVALID_ARGUMENT);
    q.size++; q.schema_version = 2;
    assert(query_record(&q, 0) == KILN_ERR_INVALID_ARGUMENT);
    q.schema_version = 1; q.flags = 8;
    assert(query_record(&q, 0) == KILN_ERR_INVALID_ARGUMENT);
    q.flags = 0; q.order = KILN_ARCHIVE_ORDER_PART;
    assert(query_record(&q, 0) == KILN_ERR_INVALID_ARGUMENT);
    q.flags = KILN_ARCHIVE_FILTER_GUID | KILN_ARCHIVE_FILTER_TYPE;
    assert(query_record(&q, 0) == KILN_OK);
    q.limit = 2;
    assert(query_record(&q, UINT64_MAX) == KILN_ERR_INVALID_ARGUMENT);
    memset(data, 0x5a, sizeof data);
    assert(content_range(UINT64_MAX, 2, data, sizeof data, &required) == KILN_ERR_INVALID_ARGUMENT);
    assert(content_range(3, UINT64_MAX, data, sizeof data, &required) == KILN_ERR_INVALID_ARGUMENT);
    assert(content_range(0, 4, data, 3, &required) == KILN_ERR_BUFFER_TOO_SMALL && required == 4);
    for (size_t i = 0; i < sizeof data; ++i) assert(data[i] == 0x5a);
    assert(content_range(0, 4, data, 4, &required) == KILN_OK);
    assert((unsigned char)data[0] == 0 && (unsigned char)data[1] == 0xff && data[2] == 0 && data[3] == 1);
    assert(content_range(4, 0, NULL, 0, &required) == KILN_OK && required == 0);
    puts("eml archive: contract probes passed (fake provider, not parity)");
    return 0;
}
