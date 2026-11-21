/* SPDX-License-Identifier: GPL-3.0-or-later
 * A native consumer of kiln-eml's real kiln.eml.archive read group, compiled against the MIT header
 * native/core/abi/services/kiln_eml_archive.h (its layout asserts included) as C11 and as C++20, into a shared library.
 * The test m6_archive_service loads it and calls m6_archive_read_consumer_run with the table kiln-eml exports and views
 * of authorized generations over the synthetic m6 archive. It checks the structural facts, record sizes and tails,
 * paging probes (nothing written on BUFFER_TOO_SMALL), exact replies, refusals, a real failure's error record copied
 * and released once, and readers on four threads. Returns the number of failed checks; each is a line in args->log. */
#include "services/kiln_eml_archive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define M6_EXPORT __declspec(dllexport)
#else
#include <pthread.h>
#define M6_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct M6ArchiveArgs {
    uint32_t size, reserved;
    const KilnEmlArchiveApiV1* api;
    KilnEmlArchiveView view;        /* a live view of OWNER's over the archive */
    KilnEmlArchiveView closed_view; /* a released view */
    KilnEmlArchiveView bare_view;   /* a live view of a generation without an archive */
    KilnEmlArchiveView broken_view; /* a live view whose resources file is gone */
    KilnOwner owner, other;
    KilnAssetKey key, missing;
    uint64_t resource_length;
    KilnContentHash content;
    uint64_t content_count, resource_count;
    const char* query;
    char* log;
    size_t log_capacity;
    uint32_t checks, failures;
} M6ArchiveArgs;

M6_EXPORT int m6_archive_read_consumer_run(M6ArchiveArgs* args);

static M6ArchiveArgs* g_args;
static size_t g_log_used;
#ifdef _WIN32
static CRITICAL_SECTION g_lock;
#define LOCK() EnterCriticalSection(&g_lock)
#define UNLOCK() LeaveCriticalSection(&g_lock)
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)
#endif

static void check(int ok, const char* what) {
    LOCK();
    ++g_args->checks;
    if (!ok) {
        size_t n = strlen(what);
        ++g_args->failures;
        if (g_args->log && g_log_used + n + 7 < g_args->log_capacity) {
            memcpy(g_args->log + g_log_used, "FAIL ", 5);
            g_log_used += 5;
            memcpy(g_args->log + g_log_used, what, n);
            g_log_used += n;
            g_args->log[g_log_used++] = '\n';
            g_args->log[g_log_used] = 0;
        }
    }
    UNLOCK();
}

static KilnArchiveErrorV1 err_rec(void) {
    KilnArchiveErrorV1 e;
    memset(&e, 0, sizeof e);
    e.size = (uint32_t)sizeof e;
    e.result = -1;
    e.token = ~(KilnHandle)0;
    e.message_bytes = ~(uint64_t)0;
    return e;
}

static KilnArchiveQuery all_query(void) {
    KilnArchiveQuery q;
    memset(&q, 0, sizeof q);
    q.size = (uint32_t)sizeof q;
    q.schema_version = KILN_EML_ARCHIVE_SCHEMA_VERSION;
    q.order = KILN_ARCHIVE_ORDER_CATALOG;
    return q;
}

static KilnStr kstr(const char* s) {
    KilnStr k;
    k.ptr = s;
    k.len = strlen(s);
    return k;
}

static unsigned char* g_resource; /* the main thread's copy */
static char* g_json;
static size_t g_json_len;

/* ---- readers on other threads ---- */
static void reader_work(void) {
    const KilnEmlArchiveApiV1* api = g_args->api;
    int i, same = 1, ok = 1;
    unsigned char* buf = (unsigned char*)malloc((size_t)g_args->resource_length);
    char* json = (char*)malloc(g_json_len);
    for (i = 0; i < 50; ++i) {
        size_t required = 0;
        KilnArchiveErrorV1 e = err_rec();
        if (api->read_resource(g_args->owner, g_args->view, &g_args->key, buf, (size_t)g_args->resource_length,
                               &required, &e) != KILN_OK)
            ok = 0;
        else if (memcmp(buf, g_resource, (size_t)g_args->resource_length) != 0)
            same = 0;
        if (api->resource_json(g_args->view, kstr(g_args->query), json, g_json_len, &required, NULL) != KILN_OK ||
            required != g_json_len)
            ok = 0;
        else if (memcmp(json, g_json, g_json_len) != 0)
            same = 0;
    }
    check(ok, "threads: every read succeeds");
    check(same, "threads: every read answers the same bytes");
    free(buf);
    free(json);
}

#ifdef _WIN32
static DWORD WINAPI reader_entry(LPVOID unused) {
    (void)unused;
    reader_work();
    return 0;
}
static void readers(void) {
    HANDLE h[4];
    int i;
    for (i = 0; i < 4; ++i) h[i] = CreateThread(NULL, 0, reader_entry, NULL, 0, NULL);
    WaitForMultipleObjects(4, h, TRUE, INFINITE);
    for (i = 0; i < 4; ++i) CloseHandle(h[i]);
}
#else
static void* reader_entry(void* unused) {
    (void)unused;
    reader_work();
    return NULL;
}
static void readers(void) {
    pthread_t t[4];
    int i;
    for (i = 0; i < 4; ++i) pthread_create(&t[i], NULL, reader_entry, NULL);
    for (i = 0; i < 4; ++i) pthread_join(t[i], NULL);
}
#endif

M6_EXPORT int m6_archive_read_consumer_run(M6ArchiveArgs* args) {
    const KilnEmlArchiveApiV1* api;
    KilnArchiveErrorV1 e;
    size_t required;
    int rc;
    g_args = args;
    g_log_used = 0;
#ifdef _WIN32
    InitializeCriticalSection(&g_lock);
#endif
    if (args->size < sizeof *args) return 1;
    api = args->api;

    /* structure: the read group and the error channel are present */
    check(api->header.size >= sizeof(KilnEmlArchiveApiV1) && api->header.version >= KILN_EML_ARCHIVE_VERSION, "header");
    check(KILN_EML_ARCHIVE_HAS(api, resource_json), "HAS resource_json");
    check(KILN_EML_ARCHIVE_HAS(api, find_resource), "HAS find_resource");
    check(KILN_EML_ARCHIVE_HAS(api, list_resources), "HAS list_resources");
    check(KILN_EML_ARCHIVE_HAS(api, read_resource), "HAS read_resource");
    check(KILN_EML_ARCHIVE_HAS(api, content_read_range), "HAS content_read_range");
    check(KILN_EML_ARCHIVE_HAS(api, list_contents), "HAS list_contents");
    check(KILN_EML_ARCHIVE_HAS(api, error_copy) && KILN_EML_ARCHIVE_HAS(api, error_release), "HAS errors");
    check(!KILN_EML_ARCHIVE_HAS(api, value_decode), "codecs stay null");
    if (args->failures) return (int)args->failures;

    /* find_resource: record sizes and tail, the key, refusals */
    {
        struct { KilnArchiveResourceInfo info; unsigned char tail[8]; } big;
        KilnArchiveResourceInfo info;
        memset(&big, 0x77, sizeof big);
        big.info.size = (uint32_t)sizeof big;
        e = err_rec();
        rc = api->find_resource(args->owner, args->view, &args->key, &big.info, &e);
        check(rc == KILN_OK && e.result == KILN_OK && e.token == 0 && e.message_bytes == 0, "find ok");
        check(big.info.size == sizeof big && big.info.schema_version == 1, "find keeps the caller's size");
        check(big.tail[0] == 0x77 && big.tail[7] == 0x77, "find leaves the tail");
        check(memcmp(big.info.key.guid, args->key.guid, 16) == 0 && big.info.key.type == args->key.type &&
                  big.info.key.part == args->key.part, "find key");
        check(big.info.source == KILN_ARCHIVE_SOURCE_FILE && big.info.byte_length == args->resource_length &&
                  big.info.reserved == 0, "find source and length");
        memset(&info, 0, sizeof info);
        info.size = (uint32_t)sizeof info - 1;
        e = err_rec();
        rc = api->find_resource(args->owner, args->view, &args->key, &info, &e);
        check(rc == KILN_ERR_INVALID_ARGUMENT && e.result == KILN_ERR_INVALID_ARGUMENT, "find short record");
        info.size = (uint32_t)sizeof info;
        e = err_rec();
        check(api->find_resource(args->owner, args->view, &args->missing, &info, &e) == KILN_ERR_NOT_FOUND &&
                  e.token == 0, "find missing");
        check(api->find_resource(args->other, args->view, &args->key, &info, &e) == KILN_ERR_REVOKED, "find foreign");
        check(api->find_resource(args->owner, args->closed_view, &args->key, &info, &e) == KILN_ERR_REVOKED, "find closed");
        check(api->find_resource(args->owner, args->bare_view, &args->key, &info, &e) == KILN_ERR_UNAVAILABLE, "find bare");
        check(api->find_resource(args->owner, args->view, NULL, &info, &e) == KILN_ERR_INVALID_ARGUMENT, "find NULL key");
        check(api->find_resource(0, args->view, &args->key, &info, NULL) == KILN_ERR_INVALID_ARGUMENT, "find owner 0");
    }

    /* the error record is checked before any lookup: a short one is not written and causes no work */
    e = err_rec();
    e.size = 31;
    required = 7;
    rc = api->read_resource(args->owner, args->closed_view, &args->key, NULL, 0, &required, &e);
    check(rc == KILN_ERR_INVALID_ARGUMENT && e.result == -1 && required == 7, "short record refused first");
    e = err_rec();
    e.flags = 1;
    rc = api->list_contents(args->owner, args->closed_view, 0, NULL, 0, &required, &e);
    check(rc == KILN_ERR_INVALID_ARGUMENT && e.result == -1, "flagged record refused first");

    /* read_resource: probe, short, copy */
    {
        unsigned char sentinel[4] = {0x5a, 0x5a, 0x5a, 0x5a};
        size_t n = (size_t)args->resource_length;
        g_resource = (unsigned char*)malloc(n + 1);
        e = err_rec();
        required = 0;
        rc = api->read_resource(args->owner, args->view, &args->key, sentinel, 0, &required, &e);
        check(rc == KILN_ERR_BUFFER_TOO_SMALL && required == n && e.token == 0 && sentinel[0] == 0x5a, "read probe");
        memset(g_resource, 0xa5, n + 1);
        rc = api->read_resource(args->owner, args->view, &args->key, g_resource, n - 1, &required, &e);
        check(rc == KILN_ERR_BUFFER_TOO_SMALL && required == n && g_resource[0] == 0xa5, "read one short");
        rc = api->read_resource(args->owner, args->view, &args->key, g_resource, n + 1, &required, NULL);
        check(rc == KILN_OK && required == n && g_resource[n] == 0xa5, "read copy (NULL record)");
        rc = api->read_resource(args->owner, args->view, &args->missing, g_resource, n, &required, &e);
        check(rc == KILN_ERR_NOT_FOUND, "read missing");
        rc = api->read_resource(args->owner, args->view, &args->key, NULL, 1, &required, &e);
        check(rc == KILN_ERR_INVALID_ARGUMENT && e.result == KILN_ERR_INVALID_ARGUMENT, "read NULL data with capacity");
        rc = api->read_resource(args->owner, args->view, &args->key, g_resource, n, NULL, &e);
        check(rc == KILN_ERR_INVALID_ARGUMENT, "read NULL required");
    }

    /* list_resources: the whole catalog, paged */
    {
        KilnArchiveQuery q = all_query();
        KilnAssetKey keys[8];
        size_t n = (size_t)args->resource_count;
        memset(keys, 0xcc, sizeof keys);
        e = err_rec();
        rc = api->list_resources(args->owner, args->view, &q, 0, NULL, 0, &required, &e);
        check(rc == KILN_ERR_BUFFER_TOO_SMALL && required == n, "list probe");
        rc = api->list_resources(args->owner, args->view, &q, 0, keys, n - 1, &required, &e);
        check(rc == KILN_ERR_BUFFER_TOO_SMALL && required == n && keys[0].part == 0xcccccccc, "list short");
        rc = api->list_resources(args->owner, args->view, &q, 0, keys, 8, &required, &e);
        check(rc == KILN_OK && required == n && keys[n].part == 0xcccccccc, "list all");
        rc = api->list_resources(args->owner, args->view, &q, n, keys, 8, &required, &e);
        check(rc == KILN_OK && required == 0, "list first == total");
        rc = api->list_resources(args->owner, args->view, &q, n + 1, keys, 8, &required, &e);
        check(rc == KILN_ERR_INVALID_ARGUMENT, "list first > total");
        q.limit = 1;
        rc = api->list_resources(args->owner, args->view, &q, 1, keys, 8, &required, &e);
        check(rc == KILN_OK && required == 1, "list limit");
        q.size = 47;
        rc = api->list_resources(args->owner, args->view, &q, 0, keys, 8, &required, &e);
        check(rc == KILN_ERR_INVALID_ARGUMENT, "list short query");
        q = all_query();
        q.order = KILN_ARCHIVE_ORDER_PART;
        rc = api->list_resources(args->owner, args->view, &q, 0, keys, 8, &required, &e);
        check(rc == KILN_ERR_INVALID_ARGUMENT, "list PART order without GUID and type");
        q.flags = KILN_ARCHIVE_FILTER_GUID | KILN_ARCHIVE_FILTER_TYPE;
        q.key = args->key;
        q.key.part = 0;
        rc = api->list_resources(args->owner, args->view, &q, 0, keys, 8, &required, &e);
        check(rc == KILN_OK && required == n && keys[0].part < keys[1].part && keys[1].part < keys[2].part,
              "list PART order ascending");
    }

    /* contents */
    {
        KilnContentHash hashes[8];
        unsigned char data[64];
        uint64_t size = args->content.size;
        e = err_rec();
        rc = api->list_contents(args->owner, args->view, 0, NULL, 0, &required, &e);
        check(rc == KILN_ERR_BUFFER_TOO_SMALL && required == args->content_count, "contents probe");
        rc = api->list_contents(args->owner, args->view, 0, hashes, 8, &required, &e);
        check(rc == KILN_OK && required == args->content_count, "contents list");
        rc = api->list_contents(args->owner, args->view, args->content_count + 1, hashes, 8, &required, &e);
        check(rc == KILN_ERR_INVALID_ARGUMENT, "contents first > total");
        rc = api->content_read_range(args->owner, args->view, &args->content, 0, size, NULL, 0, &required, &e);
        check(rc == KILN_ERR_BUFFER_TOO_SMALL && required == size, "content probe");
        rc = api->content_read_range(args->owner, args->view, &args->content, 0, size, data, sizeof data, &required, &e);
        check(rc == KILN_OK && required == size && data[0] == 'm', "content read");
        rc = api->content_read_range(args->owner, args->view, &args->content, size, 0, NULL, 0, &required, &e);
        check(rc == KILN_OK && required == 0, "content zero at EOF");
        rc = api->content_read_range(args->owner, args->view, &args->content, size + 1, 0, NULL, 0, &required, &e);
        check(rc == KILN_ERR_INVALID_ARGUMENT, "content start past the end");
        rc = api->content_read_range(args->owner, args->view, &args->content, 1, size, data, sizeof data, &required, &e);
        check(rc == KILN_ERR_INVALID_ARGUMENT, "content count past the end");
    }

    /* resource_json */
    {
        char probe[2] = {'x', 'x'};
        e = err_rec();
        rc = api->resource_json(args->view, kstr(args->query), probe, 0, &required, &e);
        check(rc == KILN_ERR_BUFFER_TOO_SMALL && probe[0] == 'x' && e.token == 0, "json probe");
        g_json_len = required;
        g_json = (char*)malloc(g_json_len);
        rc = api->resource_json(args->view, kstr(args->query), g_json, g_json_len, &required, &e);
        check(rc == KILN_OK && required == g_json_len, "json copy");
        check(g_json_len > 20 && memcmp(g_json, "{\"schema_version\":1,\"pin\":{", 27) == 0, "json envelope");
        check(api->resource_json(args->view, kstr("item:a..b"), NULL, 0, &required, &e) == KILN_ERR_INVALID_ARGUMENT,
              "json malformed query");
        check(api->resource_json(args->view, kstr("item:Sword"), NULL, 0, &required, &e) == KILN_ERR_UNAVAILABLE,
              "json no item index");
        check(api->resource_json(args->bare_view, kstr(args->query), NULL, 0, &required, &e) == KILN_ERR_UNAVAILABLE,
              "json bare generation");
        check(api->resource_json(args->closed_view, kstr(args->query), NULL, 0, &required, &e) == KILN_ERR_REVOKED,
              "json closed view");
    }

    /* a real failure: its record, copied and released once */
    {
        char text[512];
        size_t need = 0;
        KilnHandle token;
        e = err_rec();
        rc = api->read_resource(args->owner, args->broken_view, &args->key, g_resource, (size_t)args->resource_length,
                                &required, &e);
        check(rc == KILN_ERR_INTERNAL && e.result == KILN_ERR_INTERNAL && e.token != 0 && e.message_bytes > 0,
              "failure record");
        token = e.token;
        rc = api->error_copy(args->owner, token, text, 0, &need);
        check(rc == KILN_ERR_BUFFER_TOO_SMALL && need == e.message_bytes, "error probe");
        rc = api->error_copy(args->owner, token, text, sizeof text, &need);
        check(rc == KILN_OK && need == e.message_bytes && memcmp(text, "could not open", 14) == 0, "error copy");
        check(api->error_copy(args->other, token, text, sizeof text, &need) == KILN_ERR_REVOKED, "error foreign copy");
        check(api->error_release(args->owner, token) == KILN_OK, "error release");
        check(api->error_release(args->owner, token) == KILN_OK, "error release again");
        check(api->error_copy(args->owner, token, text, sizeof text, &need) == KILN_ERR_REVOKED, "error after release");
        check(api->error_release(args->owner, 0) == KILN_ERR_INVALID_ARGUMENT, "error release zero");
    }

    readers();
    free(g_resource);
    free(g_json);
#ifdef _WIN32
    DeleteCriticalSection(&g_lock);
#endif
    return (int)args->failures;
}

#ifdef __cplusplus
}
#endif
