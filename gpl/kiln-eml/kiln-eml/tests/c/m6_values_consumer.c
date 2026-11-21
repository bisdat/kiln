/* SPDX-License-Identifier: GPL-3.0-or-later
 * A native consumer of kiln-eml's real kiln.eml.values table, compiled against the MIT header
 * native/core/abi/services/kiln_eml_values.h (its layout asserts included) as C11 and as C++20, into a shared library.
 * The test m6_value_abi loads it and calls m6_values_consumer_run with the table kiln-eml published as kiln.eml.values
 * and live archive views of authorized generations over the synthetic m6 registry. Its protected reader is a small
 * object store in C (tables with string keys and a sequence part, strings, integers, proxies): no Lua.
 * It checks records, refusals, the fixed replies, packets copied with a size probe first, threads (WRONG_THREAD,
 * any-thread close with wake), a truncated reader that lacks wake, and the close/collect drain.
 * Returns the number of failed checks; each failure is a line in args->log. */
#include "services/kiln_eml_values.h"
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

typedef struct M6ConsumerArgs {
    uint32_t size, reserved;
    const KilnEmlValuesApiV1* api;
    KilnEmlArchiveView view;          /* a live view of a generation that keeps its registry */
    KilnEmlArchiveView closed_view;   /* a released view */
    KilnEmlArchiveView mismatch_view; /* a live view whose generation keeps another registry */
    KilnEmlArchiveView bare_view;     /* a live view whose generation keeps no registry */
    KilnOwner owner;
    char* log;
    size_t log_capacity;
    uint32_t checks, failures;
} M6ConsumerArgs;

M6_EXPORT int m6_values_consumer_run(M6ConsumerArgs* args);

static M6ConsumerArgs* g_args;
static size_t g_log_used;

static void check(int ok, const char* what) {
    ++g_args->checks;
    if (!ok) {
        ++g_args->failures;
        if (g_args->log && g_log_used + strlen(what) + 2 < g_args->log_capacity) {
            memcpy(g_args->log + g_log_used, "FAIL ", 5);
            g_log_used += 5;
            memcpy(g_args->log + g_log_used, what, strlen(what));
            g_log_used += strlen(what);
            g_args->log[g_log_used++] = '\n';
            g_args->log[g_log_used] = 0;
        }
    }
}

/* ---- a protected reader over a small object store ---- */
enum { O_NIL, O_BOOL, O_INT, O_STR, O_TABLE, O_PROXY };
#define MAX_OBJ 4096
#define MAX_KEYS 8
typedef struct Obj {
    int kind;
    int64_t i;
    const char* s;
    int nkeys;
    const char* keys[MAX_KEYS];
    uint64_t vals[MAX_KEYS];
    int nseq;
    uint64_t seq[MAX_KEYS];
    uint64_t lease, node;
    int retained;
} Obj;
static Obj g_obj[MAX_OBJ];
static int g_nobj;
static int g_in_access, g_releases_in_access, g_stray, g_retains, g_releases, g_accesses, g_adopted, g_reused;
static volatile long g_wakes;
static uint64_t g_wake_arena;

static uint64_t obj(int kind) {
    if (g_nobj >= MAX_OBJ) return 0;
    memset(&g_obj[g_nobj], 0, sizeof g_obj[g_nobj]);
    g_obj[g_nobj].kind = kind;
    return (uint64_t)(++g_nobj);
}
static Obj* at(uint64_t token) { return token >= 1 && token <= (uint64_t)g_nobj ? &g_obj[token - 1] : NULL; }
static uint64_t int_obj(int64_t i) { uint64_t t = obj(O_INT); at(t)->i = i; return t; }
static uint64_t str_obj(const char* s) { uint64_t t = obj(O_STR); at(t)->s = s; return t; }
static uint64_t table_obj(void) { return obj(O_TABLE); }
static void set(uint64_t table, const char* key, uint64_t value) {
    Obj* t = at(table);
    t->keys[t->nkeys] = key;
    t->vals[t->nkeys++] = value;
}
static void push(uint64_t table, uint64_t value) { Obj* t = at(table); t->seq[t->nseq++] = value; }
static uint64_t child(int64_t base, int64_t extra) {
    uint64_t t = table_obj();
    set(t, "base", int_obj(base));
    set(t, "extra", int_obj(extra));
    return t;
}

static void describe(uint64_t token, KilnEmlLuaValueV1* out) {
    Obj* o = at(token);
    out->object = token;
    switch (o->kind) {
    case O_NIL: out->kind = KILN_LUA_NIL; break;
    case O_BOOL: out->kind = KILN_LUA_BOOLEAN; out->bits = (uint64_t)o->i; break;
    case O_INT: out->kind = KILN_LUA_INTEGER; out->bits = (uint64_t)o->i; break;
    case O_STR: out->kind = KILN_LUA_STRING; out->byte_length = strlen(o->s); break;
    case O_TABLE: out->kind = KILN_LUA_TABLE; break;
    default: out->kind = KILN_LUA_TYPED_VALUE; out->typed_value = o->lease; break;
    }
}

static KilnResult KILN_CALL r_access(void* user, const KilnEmlLuaAccessV1* a, KilnEmlLuaValueV1* out) {
    Obj* o;
    int k;
    (void)user;
    ++g_accesses;
    g_in_access = 1;
    if (a->size != sizeof(KilnEmlLuaAccessV1) || out->size != sizeof(KilnEmlLuaValueV1)) { g_in_access = 0; return KILN_ERR_INVALID_ARGUMENT; }
    switch (a->operation) {
    case KILN_LUA_DESCRIBE:
        if (!at(a->object)) { g_in_access = 0; return KILN_ERR_NOT_FOUND; }
        describe(a->object, out);
        break;
    case KILN_LUA_GET_FIELD_VALUE:
    case KILN_LUA_GET_FIELD_STRING:
        o = at(a->object);
        if (!o || o->kind != O_TABLE) { g_in_access = 0; return KILN_ERR_INVALID_ARGUMENT; }
        describe(obj(O_NIL), out);
        for (k = 0; k < o->nkeys; ++k)
            if (strlen(o->keys[k]) == a->key.len && memcmp(o->keys[k], a->key.ptr, a->key.len) == 0) describe(o->vals[k], out);
        break;
    case KILN_LUA_LENGTH:
        o = at(a->object);
        if (!o || o->kind != O_TABLE) { g_in_access = 0; return KILN_ERR_INVALID_ARGUMENT; }
        describe(int_obj(o->nseq), out);
        break;
    case KILN_LUA_RAW_SEQUENCE_INDEX:
        o = at(a->object);
        if (!o || o->kind != O_TABLE) { g_in_access = 0; return KILN_ERR_INVALID_ARGUMENT; }
        describe(a->index >= 1 && a->index <= o->nseq ? o->seq[a->index - 1] : obj(O_NIL), out);
        break;
    case KILN_LUA_SOURCE_TO_STRING:
        describe(str_obj("<c object>"), out);
        break;
    case KILN_LUA_MAKE_TYPED_PROXY:
        for (k = 0; k < g_nobj; ++k)
            if (g_obj[k].kind == O_PROXY && g_obj[k].node == a->node_id) {
                describe((uint64_t)(k + 1), out);
                out->lease_disposition = KILN_LUA_LEASE_REUSED;
                ++g_reused;
                g_in_access = 0;
                return KILN_OK;
            }
        {
            uint64_t t = obj(O_PROXY);
            at(t)->lease = a->object;
            at(t)->node = a->node_id;
            describe(t, out);
            out->lease_disposition = KILN_LUA_LEASE_ADOPTED;
            ++g_adopted;
        }
        break;
    default: g_in_access = 0; return KILN_ERR_INVALID_ARGUMENT;
    }
    g_in_access = 0;
    return KILN_OK;
}
static KilnResult KILN_CALL r_copy(void* user, uint64_t object, void* out, size_t capacity, size_t* required) {
    Obj* o = at(object);
    size_t n;
    (void)user;
    if (!o || o->kind != O_STR) return KILN_ERR_INVALID_ARGUMENT;
    n = strlen(o->s);
    *required = n;
    if (capacity < n) return KILN_ERR_BUFFER_TOO_SMALL;
    memcpy(out, o->s, n);
    return KILN_OK;
}
static KilnResult KILN_CALL r_retain(void* user, uint64_t object) {
    (void)user;
    if (!at(object)) return KILN_ERR_NOT_FOUND;
    ++at(object)->retained;
    ++g_retains;
    return KILN_OK;
}
static void KILN_CALL r_release(void* user, uint64_t object) {
    (void)user;
    ++g_releases;
    if (g_in_access) ++g_releases_in_access;
    if (!at(object) || at(object)->retained <= 0) { ++g_stray; return; }
    --at(object)->retained;
}
static void KILN_CALL r_wake(void* user, KilnHandle arena) {
    (void)user;
    g_wake_arena = arena;
#ifdef _WIN32
    InterlockedIncrement(&g_wakes);
#else
    __atomic_fetch_add(&g_wakes, 1, __ATOMIC_SEQ_CST);
#endif
}
static long wakes(void) {
#ifdef _WIN32
    return InterlockedCompareExchange(&g_wakes, 0, 0);
#else
    return __atomic_load_n(&g_wakes, __ATOMIC_SEQ_CST);
#endif
}

/* ---- another thread ---- */
typedef struct Job { int what; KilnHandle token; KilnResult rc; } Job;
static const KilnEmlValuesApiV1* g_api;
static KilnHandle g_arena;
static void job_run(Job* j) {
    KilnEmlValueResultV1 r;
    KilnEmlValueOpenV1 open;
    uint32_t pending = 0;
    memset(&r, 0, sizeof r);
    r.size = sizeof r;
    memset(&open, 0, sizeof open);
    open.size = sizeof open;
    open.origin = KILN_VALUE_OPEN_DEFAULT;
    open.qualified_type.ptr = "m6.Child";
    open.qualified_type.len = 8;
    if (j->what == 0) j->rc = g_api->value_open(g_args->owner, g_arena, g_args->view, &open, &r);
    else if (j->what == 1) j->rc = g_api->value_close(g_args->owner, j->token);
    else if (j->what == 2) j->rc = g_api->arena_collect(g_args->owner, g_arena, &pending);
    else j->rc = g_api->arena_close(g_args->owner, j->token, &pending);
}
#ifdef _WIN32
static DWORD WINAPI job_entry(LPVOID p) { job_run((Job*)p); return 0; }
static void on_thread(Job* j) {
    HANDLE h = CreateThread(NULL, 0, job_entry, j, 0, NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}
#else
static void* job_entry(void* p) { job_run((Job*)p); return NULL; }
static void on_thread(Job* j) {
    pthread_t t;
    pthread_create(&t, NULL, job_entry, j);
    pthread_join(t, NULL);
}
#endif

/* ---- helpers ---- */
static KilnEmlValueResultV1 result(void) {
    KilnEmlValueResultV1 r;
    memset(&r, 0xcd, sizeof r);
    r.size = sizeof r;
    return r;
}
static KilnEmlValueSelectorV1 field(const char* name) {
    KilnEmlValueSelectorV1 s;
    memset(&s, 0, sizeof s);
    s.size = sizeof s;
    s.kind = KILN_VALUE_FIELD;
    s.field.ptr = name;
    s.field.len = strlen(name);
    return s;
}
static KilnEmlValueOpenV1 lua_input(const char* type, uint64_t object) {
    KilnEmlValueOpenV1 o;
    memset(&o, 0, sizeof o);
    o.size = sizeof o;
    o.origin = KILN_VALUE_OPEN_LUA_INPUT;
    o.qualified_type.ptr = type;
    o.qualified_type.len = strlen(type);
    o.lua_object = object;
    return o;
}
/* A packet's bytes into buf (NUL-terminated): a size probe with capacity 0, then the copy. */
static size_t packet(KilnHandle token, char* buf, size_t cap) {
    size_t required = 0, again = 0;
    KilnResult rc = g_api->value_result_copy(g_args->owner, token, NULL, 0, &required);
    check(rc == KILN_ERR_BUFFER_TOO_SMALL || (rc == KILN_OK && required == 0), "a size probe answers BUFFER_TOO_SMALL with the size");
    if (required + 1 > cap) return 0;
    rc = g_api->value_result_copy(g_args->owner, token, buf, required, &again);
    check(rc == KILN_OK && again == required, "the copy at the probed size succeeds");
    buf[required] = 0;
    return required;
}
static KilnEmlLuaReaderV1 reader(void) {
    KilnEmlLuaReaderV1 r;
    memset(&r, 0, sizeof r);
    r.size = sizeof r;
    r.version = 1;
    r.user = &g_obj;
    r.access = r_access;
    r.copy = r_copy;
    r.retain = r_retain;
    r.release = r_release;
    r.wake = r_wake;
    return r;
}

#define HAS(api, m) ((api)->header.size >= offsetof(KilnEmlValuesApiV1, m) + sizeof(void*) && (api)->m != NULL)

int m6_values_consumer_run(M6ConsumerArgs* args) {
    const KilnEmlValuesApiV1* api;
    KilnEmlLuaReaderV1 rd;
    KilnHandle arena = 99, trunc = 0;
    KilnEmlValueResultV1 r, s;
    KilnEmlValueOpenV1 open;
    KilnEmlValueSelectorV1 sel;
    KilnEmlValueInfoV1 info;
    unsigned char big[64];
    char buf[4096];
    uint32_t pending = 99;
    KilnHandle root, kids, base_payload;
    int accesses;
    long w;
    Job job;
    size_t required = 0;
    if (!args || args->size < sizeof *args || !args->api) return -1;
    g_args = args;
    g_log_used = 0;
    g_api = api = args->api;
    args->checks = args->failures = 0;
    if (args->log && args->log_capacity) args->log[0] = 0;

    /* the table */
    check(api->header.version == KILN_EML_VALUES_VERSION && api->header.size == sizeof(KilnEmlValuesApiV1),
          "v1 table of the header's 144 bytes");
    check(HAS(api, arena_create) && HAS(api, arena_close) && HAS(api, arena_collect) && HAS(api, value_open) &&
              HAS(api, value_info) && HAS(api, value_read) && HAS(api, value_child) && HAS(api, value_set) &&
              HAS(api, value_clone) && HAS(api, value_export) && HAS(api, value_close) &&
              HAS(api, value_result_copy) && HAS(api, value_array_push) && HAS(api, value_array_insert) &&
              HAS(api, value_array_pop) && HAS(api, value_array_remove) && HAS(api, value_array_clear),
          "all seventeen members are present");

    /* arena_create refusals: nothing is created, the token stays 0 */
    rd = reader();
    check(api->arena_create(args->owner, args->view, KILN_VALUE_RUNTIME, NULL, &arena) == KILN_ERR_INVALID_ARGUMENT && arena == 0,
          "no reader: INVALID_ARGUMENT, token 0");
    check(api->arena_create(args->owner, args->view, KILN_VALUE_RUNTIME, &rd, NULL) == KILN_ERR_INVALID_ARGUMENT, "no output: INVALID_ARGUMENT");
    check(api->arena_create(0, args->view, KILN_VALUE_RUNTIME, &rd, &arena) == KILN_ERR_INVALID_ARGUMENT, "host owner 0 refused");
    check(api->arena_create(args->owner, args->view, 7, &rd, &arena) == KILN_ERR_INVALID_ARGUMENT, "unknown phase refused");
    rd.size = 40; /* no release */
    check(api->arena_create(args->owner, args->view, KILN_VALUE_RUNTIME, &rd, &arena) == KILN_ERR_INVALID_ARGUMENT, "a reader without release is refused");
    rd = reader();
    rd.version = 2;
    check(api->arena_create(args->owner, args->view, KILN_VALUE_RUNTIME, &rd, &arena) == KILN_ERR_INVALID_ARGUMENT, "reader version 2 refused");
    rd = reader();
    rd.copy = NULL;
    check(api->arena_create(args->owner, args->view, KILN_VALUE_RUNTIME, &rd, &arena) == KILN_ERR_INVALID_ARGUMENT, "a null copy is refused");
    rd = reader();
    check(api->arena_create(args->owner, 0x7fffffff, KILN_VALUE_RUNTIME, &rd, &arena) == KILN_ERR_INVALID_ARGUMENT, "unknown view: INVALID_ARGUMENT");
    check(api->arena_create(args->owner, args->closed_view, KILN_VALUE_RUNTIME, &rd, &arena) == KILN_ERR_REVOKED, "closed view: REVOKED");
    check(api->arena_create(args->owner, args->mismatch_view, KILN_VALUE_RUNTIME, &rd, &arena) == KILN_ERR_UNAVAILABLE && arena == 0,
          "a generation whose registry is not its snapshot's: UNAVAILABLE");
    check(api->arena_create(args->owner, args->bare_view, KILN_VALUE_RUNTIME, &rd, &arena) == KILN_ERR_UNAVAILABLE, "a generation without a registry: UNAVAILABLE");
    check(g_accesses == 0, "no refused arena_create called the reader");

    /* a real arena */
    check(api->arena_create(args->owner, args->view, KILN_VALUE_RUNTIME, &rd, &arena) == KILN_OK && arena != 0, "arena_create");
    g_arena = arena;
    memset(&rd, 0, sizeof rd); /* the provider keeps its own snapshot */

    /* value_open LUA_INPUT m6.Child {base = 7, extra = 11} */
    root = child(7, 11);
    open = lua_input("m6.Child", root);
    r = result();
    r.size = sizeof r - 1;
    accesses = g_accesses;
    check(api->value_open(args->owner, arena, args->view, &open, &r) == KILN_ERR_INVALID_ARGUMENT && g_accesses == accesses,
          "a short result record: INVALID_ARGUMENT, no reader call");
    r = result();
    open.size = sizeof open - 1;
    check(api->value_open(args->owner, arena, args->view, &open, &r) == KILN_ERR_INVALID_ARGUMENT && r.item == 0 && r.disposition == 0,
          "a short open record: INVALID_ARGUMENT, the result cleared");
    open = lua_input("m6.Child", root);
    open.origin = 9;
    check(api->value_open(args->owner, arena, args->view, &open, &r) == KILN_ERR_INVALID_ARGUMENT, "unknown origin refused");
    open = lua_input("m6.Child", root);
    check(api->value_open(args->owner, arena, args->closed_view, &open, &r) == KILN_ERR_REVOKED, "a closed owner view: REVOKED");
    check(api->value_open(args->owner, arena, args->mismatch_view, &open, &r) == KILN_ERR_INVALID_ARGUMENT, "another generation's view: INVALID_ARGUMENT");
    open.qualified_type.ptr = NULL;
    check(api->value_open(args->owner, arena, args->view, &open, &r) == KILN_ERR_INVALID_ARGUMENT, "a null type text with a length refused");
    open = lua_input("m6.Child", root);
    memset(big, 0xab, sizeof big);
    ((KilnEmlValueResultV1*)(void*)big)->size = sizeof big;
    check(api->value_open(args->owner, arena, args->view, &open, (KilnEmlValueResultV1*)(void*)big) == KILN_OK, "value_open LUA_INPUT");
    memcpy(&r, big, sizeof r);
    check(r.size == sizeof big && big[48] == 0xab && big[63] == 0xab, "a larger result record keeps its size and its tail");
    check(r.disposition == KILN_VALUE_HANDLED && r.arity == 1 && r.item != 0 && r.payload == 0 && r.error == 0 && r.flags == 0,
          "open: HANDLED, arity 1, one item");
    root = r.item;

    /* value_info */
    memset(&info, 0, sizeof info);
    info.size = sizeof info - 1;
    check(api->value_info(args->owner, root, &info) == KILN_ERR_INVALID_ARGUMENT, "a short info record refused");
    info.size = sizeof info;
    info.requested = KILN_VALUE_INFO_LENGTH | KILN_VALUE_INFO_DIRTY;
    check(api->value_info(args->owner, root, &info) == KILN_OK, "value_info");
    check(info.kind == KILN_VALUE_STRUCT_OWNED && info.identity.size == sizeof(KilnEmlValueIdentityV1) &&
              info.identity.lua_phase == KILN_VALUE_RUNTIME && info.identity.arena != 0 && info.identity.arena != arena &&
              info.identity.archive.size == sizeof(KilnEmlArchiveIdentityV1) &&
              info.identity.layout_revision == info.identity.archive.registry_id && info.logical_len == 2 &&
              (info.flags & KILN_VALUE_DIRTY) != 0,
          "identity: phase, a public arena id that is not the control, the view's pins, layout_revision = registry_id");

    /* value_child: a scalar is item 0 plus its VIEW */
    sel = field("base");
    sel.size = sizeof sel - 1;
    r = result();
    check(api->value_child(args->owner, root, &sel, &r) == KILN_ERR_INVALID_ARGUMENT, "a short selector refused");
    sel = field("base");
    sel.kind = 9;
    check(api->value_child(args->owner, root, &sel, &r) == KILN_ERR_INVALID_ARGUMENT, "an unknown selector kind refused");
    sel.kind = KILN_VALUE_INDEX;
    check(api->value_child(args->owner, root, &sel, &r) == KILN_ERR_INVALID_ARGUMENT, "an INDEX selector on a struct refused");
    sel = field("base");
    check(api->value_child(args->owner, root, &sel, &r) == KILN_OK && r.item == 0 && r.payload != 0, "child base: a VIEW payload");
    base_payload = r.payload;
    check(api->value_result_copy(args->owner, base_payload, NULL, 0, NULL) == KILN_ERR_INVALID_ARGUMENT, "no required output refused");
    check(api->value_result_copy(args->owner, base_payload, NULL, 8, &required) == KILN_ERR_INVALID_ARGUMENT, "a null buffer with capacity refused");
    check(api->value_result_copy(args->owner, base_payload, buf, 4, &required) == KILN_ERR_BUFFER_TOO_SMALL && required > 4,
          "a short buffer: BUFFER_TOO_SMALL with the size");
    check(packet(base_payload, buf, sizeof buf) > 0 && strstr(buf, "\"i64:0000000000000007\"") != NULL, "the VIEW carries base = 7");
    check(api->value_close(args->owner, base_payload) == KILN_OK && api->value_close(args->owner, base_payload) == KILN_OK,
          "close is idempotent");
    check(api->value_result_copy(args->owner, base_payload, buf, sizeof buf, &required) == KILN_ERR_REVOKED, "a closed packet: REVOKED");

    /* value_set: MUTATED, arity 0; a wrong type is a SEMANTIC_ERROR with an error lease */
    r = result();
    check(api->value_set(args->owner, root, &sel, int_obj(9), &r) == KILN_OK && r.disposition == KILN_VALUE_HANDLED &&
              (r.flags & KILN_VALUE_MUTATED) && r.arity == 0 && r.receiver_revision == 1,
          "set base = 9: MUTATED, arity 0, revision 1");
    r = result();
    check(api->value_set(args->owner, root, &sel, str_obj("x"), &r) == KILN_OK && r.disposition == KILN_VALUE_SEMANTIC_ERROR &&
              r.error != 0 && r.flags == 0 && r.item == 0,
          "set base = \"x\": SEMANTIC_ERROR, never MUTATED");
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    check(api->value_info(args->owner, r.error, &info) == KILN_OK && info.kind == KILN_VALUE_ERROR, "the error lease is an ERROR");
    check(packet(r.error, buf, sizeof buf) > 0 && strstr(buf, "\"class\":\"validation\"") && strstr(buf, "\"kind\":\"incompatible_type\""),
          "the error packet: a validation tree");
    api->value_close(args->owner, r.error);
    r = result();
    check(api->value_child(args->owner, root, &sel, &r) == KILN_OK && packet(r.payload, buf, sizeof buf) > 0 &&
              strstr(buf, "\"i64:0000000000000009\""),
          "the failed set left base = 9");
    api->value_close(args->owner, r.payload);

    /* arrays: m6.Children {{base = 1, extra = 2}} */
    kids = table_obj();
    push(kids, child(1, 2));
    open = lua_input("m6.Children", kids);
    r = result();
    check(api->value_open(args->owner, arena, args->view, &open, &r) == KILN_OK && r.item != 0, "open m6.Children");
    kids = r.item;
    r = result();
    check(api->value_array_push(args->owner, kids, child(3, 4), &r) == KILN_OK && (r.flags & KILN_VALUE_MUTATED) && r.arity == 1,
          "push: MUTATED, arity 1");
    r = result();
    check(api->value_array_insert(args->owner, kids, 1, child(5, 6), &r) == KILN_OK && (r.flags & KILN_VALUE_MUTATED), "insert at 1");
    r = result();
    check(api->value_array_insert(args->owner, kids, 9, child(5, 6), &r) == KILN_OK && r.disposition == KILN_VALUE_SEMANTIC_ERROR,
          "insert at 9: position out of bounds");
    check(packet(r.error, buf, sizeof buf) > 0 && strstr(buf, "\"class\":\"position_out_of_bounds\""), "the error class");
    api->value_close(args->owner, r.error);
    r = result();
    check(api->value_read(args->owner, kids, KILN_VALUE_READ_LENGTH, &r) == KILN_OK && packet(r.payload, buf, sizeof buf) > 0 &&
              strstr(buf, "\"i64:0000000000000003\""),
          "LENGTH 3");
    api->value_close(args->owner, r.payload);
    r = result();
    check(api->value_array_pop(args->owner, kids, &r) == KILN_OK && r.item != 0 && (r.flags & KILN_VALUE_MUTATED), "pop: the removed node");
    api->value_close(args->owner, r.item);
    r = result();
    check(api->value_array_remove(args->owner, kids, 0, &r) == KILN_OK && r.item == 0 && r.flags == 0 && r.arity == 1,
          "remove(0): nothing removed, nothing changed");
    r = result();
    check(api->value_array_push(args->owner, root, child(1, 1), &r) == KILN_OK && r.disposition == KILN_VALUE_UNSUPPORTED && r.arity == 0,
          "push on a struct: UNSUPPORTED");
    r = result();
    check(api->value_array_clear(args->owner, kids, &r) == KILN_OK && (r.flags & KILN_VALUE_MUTATED), "clear: MUTATED");

    /* children through proxies: MAKE_TYPED_PROXY adopts once, then reuses */
    kids = table_obj();
    push(kids, child(1, 2));
    open = lua_input("m6.Children", kids);
    r = result();
    check(api->value_open(args->owner, arena, args->view, &open, &r) == KILN_OK, "open m6.Children again");
    kids = r.item;
    sel.kind = KILN_VALUE_INDEX;
    sel.index = 1;
    sel.field.ptr = NULL;
    sel.field.len = 0;
    r = result();
    s = result();
    check(api->value_child(args->owner, kids, &sel, &r) == KILN_OK && r.item != 0 && api->value_child(args->owner, kids, &sel, &s) == KILN_OK &&
              s.item != 0 && g_adopted == 1 && g_reused == 0,
          "a typed child: its slot pins one adopted proxy");
    api->value_close(args->owner, r.item);
    api->value_close(args->owner, s.item);

    /* STRINGIFY, clone, export, iteration */
    r = result();
    check(api->value_read(args->owner, root, KILN_VALUE_READ_STRINGIFY, &r) == KILN_OK && packet(r.payload, buf, sizeof buf) > 0 && buf[0] == '{',
          "STRINGIFY: raw text");
    api->value_close(args->owner, r.payload);
    r = result();
    check(api->value_clone(args->owner, root, &r) == KILN_OK && r.item != 0 && r.item != root, "clone");
    api->value_close(args->owner, r.item);
    r = result();
    {
        KilnStr none;
        none.ptr = NULL;
        none.len = 0;
        check(api->value_export(args->owner, root, none, &r) == KILN_OK && r.payload != 0 && packet(r.payload, buf, sizeof buf) == 8,
              "export: 8 bytes of m6.Child");
        api->value_close(args->owner, r.payload);
    }
    r = result();
    check(api->value_read(args->owner, root, KILN_VALUE_READ_ITER_BEGIN, &r) == KILN_OK && r.item != 0 && r.arity == 3, "ITER_BEGIN");
    s = result();
    check(api->value_read(args->owner, r.item, KILN_VALUE_READ_ITER_NEXT, &s) == KILN_OK && s.arity == 2 && s.payload != 0, "ITER_NEXT");
    api->value_close(args->owner, s.payload);
    api->value_close(args->owner, r.item);

    /* threads: worker-only members refuse elsewhere; close is any-thread and wakes the host with the control */
    job.what = 0;
    on_thread(&job);
    check(job.rc == KILN_ERR_WRONG_THREAD, "value_open off the worker: WRONG_THREAD");
    job.what = 2;
    on_thread(&job);
    check(job.rc == KILN_ERR_WRONG_THREAD, "arena_collect off the worker: WRONG_THREAD");
    w = wakes();
    r = result();
    check(api->value_clone(args->owner, root, &r) == KILN_OK, "a lease to close elsewhere");
    job.what = 1;
    job.token = r.item;
    on_thread(&job);
    check(job.rc == KILN_OK && wakes() == w + 1 && g_wake_arena == arena, "value_close off the worker: OK, one wake with the control");
    check(api->value_close(args->owner, r.item) == KILN_OK, "closing it again on the worker: OK");
    w = wakes();
    r = result();
    check(api->value_clone(args->owner, root, &r) == KILN_OK && api->value_close(args->owner, r.item) == KILN_OK && wakes() == w,
          "a close on the worker does not wake");
    check(api->arena_collect(args->owner, arena, &pending) == KILN_OK, "collect on the worker");

    /* a reader that lacks wake (48 bytes): admitted, and never signalled */
    rd = reader();
    rd.size = offsetof(KilnEmlLuaReaderV1, wake);
    check(api->arena_create(args->owner, args->view, KILN_VALUE_RUNTIME, &rd, &trunc) == KILN_OK && trunc != 0, "a reader without wake is admitted");
    open = lua_input("m6.Child", child(1, 2));
    r = result();
    check(api->value_open(args->owner, trunc, args->view, &open, &r) == KILN_OK, "open in that arena");
    w = wakes();
    job.what = 1;
    job.token = r.item;
    on_thread(&job);
    check(job.rc == KILN_OK && wakes() == w, "its off-worker close signals nothing");
    job.what = 3;
    job.token = trunc;
    on_thread(&job);
    check(job.rc == KILN_OK && wakes() == w, "nor does its off-worker arena_close");
    check(api->arena_collect(args->owner, trunc, &pending) == KILN_OK && pending == 0, "its collect drains to 0");

    /* close and drain: then everything refuses or stays idempotent */
    check(api->arena_close(args->owner, arena, &pending) == KILN_OK && pending == 0, "arena_close on the worker drains at once");
    check(api->arena_close(args->owner, arena, NULL) == KILN_OK, "arena_close again (no pending output)");
    check(api->arena_collect(args->owner, arena, &pending) == KILN_OK && pending == 0, "collect: 0");
    r = result();
    check(api->value_child(args->owner, root, &sel, &r) == KILN_ERR_REVOKED, "a lease of a closed arena: REVOKED");
    check(api->value_close(args->owner, root) == KILN_OK, "its close stays idempotent");
    check(api->value_close(args->owner, UINT64_MAX) == KILN_ERR_NOT_FOUND, "a never-issued token: NOT_FOUND");
    check(api->value_close(args->owner, arena) == KILN_ERR_INVALID_ARGUMENT || api->value_close(args->owner, arena) == KILN_ERR_REVOKED,
          "the control is not a lease");
    check(g_retains == g_releases && g_stray == 0 && g_releases_in_access == 0,
          "every retain released once, never inside a reader call");
    return (int)args->failures;
}

#ifdef __cplusplus
}
#endif
