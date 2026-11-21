/* SPDX-License-Identifier: MIT
 * Contract probes for kiln.eml.startup v1 (published_identity appended later in v1). The
 * placeholders below are NOT the GPL provider (kiln-eml) and prove nothing about the witness,
 * the barrier, the outcome or the published identity. */
#ifdef PROBE_ARCHIVE_FIRST
/* The canonical archive header first: a second definition of its identity record in the
 * startup header would fail this translation unit. */
#include "services/kiln_eml_archive.h"
#endif
#include "services/kiln_eml_startup.h"
/* PROBE_LAYOUT_ONLY: the static layout, enum and signature asserts alone, with no C library,
 * for the Windows x64 compile-only check in test-abi-headers.sh. */
#ifndef PROBE_LAYOUT_ONLY
#include <assert.h>
#include <stdio.h>
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
#define W(t, m, n) PROBE_ASSERT(sizeof(((t*)0)->m) == n, #t "." #m " width")

SIZE(KilnEmlStartupKeyV1, 56);
SIZE(KilnEmlStartupWitnessV1, 40);
SIZE(KilnEmlStartupApiV1, 56);
SIZE(KilnEmlArchiveIdentityV1, 160);
O(KilnEmlStartupKeyV1, size, 0); O(KilnEmlStartupKeyV1, flags, 4); O(KilnEmlStartupKeyV1, launch_id, 8);
O(KilnEmlStartupKeyV1, plan_sha256, 16); O(KilnEmlStartupKeyV1, session, 48);
W(KilnEmlStartupKeyV1, size, 4); W(KilnEmlStartupKeyV1, flags, 4);
W(KilnEmlStartupKeyV1, launch_id, 8); W(KilnEmlStartupKeyV1, plan_sha256, 32); W(KilnEmlStartupKeyV1, session, 8);
O(KilnEmlStartupWitnessV1, size, 0); O(KilnEmlStartupWitnessV1, state, 4); O(KilnEmlStartupWitnessV1, plan_id, 8);
O(KilnEmlStartupWitnessV1, revision, 24); O(KilnEmlStartupWitnessV1, flags, 32);
O(KilnEmlStartupWitnessV1, reserved, 36);
W(KilnEmlStartupWitnessV1, state, 4); W(KilnEmlStartupWitnessV1, plan_id, 16); W(KilnEmlStartupWitnessV1, revision, 8);
W(KilnEmlStartupWitnessV1, flags, 4); W(KilnEmlStartupWitnessV1, reserved, 4);
O(KilnEmlStartupApiV1, header, 0); O(KilnEmlStartupApiV1, attach, 8); O(KilnEmlStartupApiV1, witness_wait, 16);
O(KilnEmlStartupApiV1, witness_recheck, 24); O(KilnEmlStartupApiV1, native_terminal, 32);
O(KilnEmlStartupApiV1, outcome, 40); O(KilnEmlStartupApiV1, published_identity, 48);
PROBE_ASSERT(KILN_EML_STARTUP_VERSION == 1u, "version");
PROBE_ASSERT(KILN_EML_STARTUP_FORMAT == 1u, "JSON format");
PROBE_ASSERT(KILN_EML_STARTUP_KEY_SIZE == 56u && KILN_EML_STARTUP_WITNESS_SIZE == 40u, "v1 record sizes");
PROBE_ASSERT(KILN_EML_WITNESS_PENDING == 0 && KILN_EML_WITNESS_READY == 1 && KILN_EML_WITNESS_REFUSED == 2,
             "witness states");
PROBE_ASSERT(sizeof(KILN_EML_STARTUP_SERVICE_EXPORT) == sizeof("kiln_eml_startup_service"), "export name length");

/* Placeholders with the exact member types: assigning them pins every signature (a changed
 * parameter or return type fails to compile, in C with -Werror and in C++). */
static KilnResult KILN_CALL p_attach(const KilnEmlStartupKeyV1* key, uint32_t planned, uint64_t* session) {
    if (!key || !session || key->size < KILN_EML_STARTUP_KEY_SIZE || key->flags || key->session)
        return KILN_ERR_INVALID_ARGUMENT;
    (void)planned; *session = 77; return KILN_OK;
}
static KilnResult KILN_CALL p_wait(const KilnEmlStartupKeyV1* key, uint32_t timeout_ms, KilnEmlStartupWitnessV1* out,
                                   char* reason, size_t capacity, size_t* required) {
    (void)key; (void)timeout_ms; (void)reason; (void)capacity;
    if (!out || !required) return KILN_ERR_INVALID_ARGUMENT;
    out->state = KILN_EML_WITNESS_READY; out->revision = 1; *required = 0; return KILN_OK;
}
static KilnResult KILN_CALL p_recheck(const KilnEmlStartupKeyV1* key, const KilnEmlStartupWitnessV1* w,
                                      char* reason, size_t capacity, size_t* required) {
    (void)key; (void)w; (void)reason; (void)capacity; *required = 0; return KILN_OK;
}
static KilnResult KILN_CALL p_terminal(const KilnEmlStartupKeyV1* key, KilnStr json) {
    (void)key; return json.len ? KILN_OK : KILN_ERR_INVALID_ARGUMENT;
}
static KilnResult KILN_CALL p_outcome(const KilnEmlStartupKeyV1* key, KilnEmlArchiveIdentityV1* identity,
                                      char* json, size_t capacity, size_t* required) {
    (void)key; (void)identity; (void)json; (void)capacity; (void)required; return KILN_ERR_UNAVAILABLE;
}
static KilnResult KILN_CALL p_published(const KilnEmlStartupKeyV1* key, KilnEmlArchiveIdentityV1* identity) {
    if (!key || !identity || key->size < KILN_EML_STARTUP_KEY_SIZE || key->flags || key->session ||
        identity->size < sizeof *identity)
        return KILN_ERR_INVALID_ARGUMENT;
    identity->phase = KILN_EML_ARCHIVE_PUBLISHED; identity->generation = 9; return KILN_OK;
}
static const KilnEmlStartupApiV1* KILN_CALL p_service(uint32_t min_version);
static const KilnEmlStartupServiceFn probe_getter = p_service;

#ifndef PROBE_LAYOUT_ONLY
static KilnEmlStartupApiV1 table;
static const KilnEmlStartupApiV1* KILN_CALL p_service(uint32_t min_version) {
    return min_version <= KILN_EML_STARTUP_VERSION ? &table : NULL;
}
static void fill(KilnEmlStartupApiV1* api) {
    memset(api, 0, sizeof *api);
    api->header.size = sizeof *api; api->header.version = KILN_EML_STARTUP_VERSION;
    api->attach = p_attach; api->witness_wait = p_wait; api->witness_recheck = p_recheck;
    api->native_terminal = p_terminal; api->outcome = p_outcome; api->published_identity = p_published;
}

int main(void) {
    KilnEmlStartupApiV1 api;
    assert(strcmp(KILN_EML_STARTUP_SERVICE_EXPORT, "kiln_eml_startup_service") == 0);
    assert(strcmp(KILN_EML_STARTUP_SERVICE_EXPORT, KILN_EML_ARCHIVE_SERVICE_EXPORT) != 0);
    fill(&table);
    assert(probe_getter(0) == &table && probe_getter(1) == &table && probe_getter(2) == NULL);

    /* Table extents: 6 members, the last one (appended to the first five) ends at 56; a 48-byte table (a kiln-eml
     * before it) has the five and no published_identity. */
    fill(&api);
    assert(KILN_EML_STARTUP_HAS(&api, attach) && KILN_EML_STARTUP_HAS(&api, outcome) &&
           KILN_EML_STARTUP_HAS(&api, published_identity));
    api.header.size = 55;
    assert(!KILN_EML_STARTUP_HAS(&api, published_identity) && KILN_EML_STARTUP_HAS(&api, outcome));
    api.header.size = 48;
    assert(!KILN_EML_STARTUP_HAS(&api, published_identity) && KILN_EML_STARTUP_HAS(&api, outcome));
    api.header.size = sizeof api; api.published_identity = NULL;
    assert(!KILN_EML_STARTUP_HAS(&api, published_identity) && KILN_EML_STARTUP_HAS(&api, outcome));
    api.header.size = 47;
    assert(!KILN_EML_STARTUP_HAS(&api, outcome) && KILN_EML_STARTUP_HAS(&api, native_terminal));
    api.header.size = 39;
    assert(!KILN_EML_STARTUP_HAS(&api, native_terminal) && KILN_EML_STARTUP_HAS(&api, witness_recheck));
    api.header.size = 15;
    assert(!KILN_EML_STARTUP_HAS(&api, attach));
    api.header.size = sizeof api; api.header.version = 0;
    assert(!KILN_EML_STARTUP_HAS(&api, attach));
    api.header.version = 1; api.native_terminal = NULL;
    assert(!KILN_EML_STARTUP_HAS(&api, native_terminal) && KILN_EML_STARTUP_HAS(&api, outcome));
    assert(!KILN_EML_STARTUP_HAS((const KilnEmlStartupApiV1*)NULL, attach));

    /* The calls go through the declared types. */
    fill(&api);
    KilnEmlStartupKeyV1 key;
    memset(&key, 0, sizeof key);
    key.size = sizeof key; key.launch_id = 0x0123456789abcdefull;
    uint64_t session = 0;
    assert(api.attach(&key, 2, &session) == KILN_OK && session == 77);
    key.session = session;
    assert(api.attach(&key, 2, &session) == KILN_ERR_INVALID_ARGUMENT);
    KilnEmlStartupWitnessV1 w;
    memset(&w, 0, sizeof w);
    w.size = sizeof w;
    size_t required = 99;
    assert(api.witness_wait(&key, 2000, &w, NULL, 0, &required) == KILN_OK);
    assert(w.state == KILN_EML_WITNESS_READY && required == 0);
    assert(api.witness_recheck(&key, &w, NULL, 0, &required) == KILN_OK && required == 0);
    KilnStr json = { "{}", 2 };
    assert(api.native_terminal(&key, json) == KILN_OK);
    KilnEmlArchiveIdentityV1 identity;
    memset(&identity, 0, sizeof identity);
    identity.size = sizeof identity;
    assert(api.outcome(&key, &identity, NULL, 0, &required) == KILN_ERR_UNAVAILABLE);
    assert(identity.phase == 0);
    assert(api.published_identity(&key, &identity) == KILN_ERR_INVALID_ARGUMENT && identity.phase == 0);
    key.session = 0;
    assert(api.published_identity(&key, &identity) == KILN_OK && identity.phase == KILN_EML_ARCHIVE_PUBLISHED);
    puts("eml startup: contract probes passed (placeholders, not the provider)");
    return 0;
}
#else
static const KilnEmlStartupApiV1* KILN_CALL p_service(uint32_t min_version) {
    (void)min_version; (void)probe_getter; (void)p_attach; (void)p_wait; (void)p_recheck; (void)p_terminal;
    (void)p_outcome; (void)p_published; return NULL;
}
#endif /* PROBE_LAYOUT_ONLY */
