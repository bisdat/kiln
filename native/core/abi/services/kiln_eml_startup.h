/* SPDX-License-Identifier: MIT
 * kiln.eml.startup v1 -- the startup handshake between kiln-eml (provider, GPL) and
 * kiln-sfi (the one consumer, MIT) in a kiln-backend start: kiln-sfi attaches, waits for
 * kiln-eml's startup witness before any package DLL loads, rechecks it after the DLLs,
 * pushes its native terminal, and reads kiln-eml's startup outcome together with the
 * archive identity of the generation it may be granted. Declarations only: no provider,
 * no registration: kiln-eml provides it, kiln-sfi consumes it.
 *
 * Kiln-internal: the startup seam of Kiln's interpreter for ShroudForge mods, not in the
 * SDK's headers and no mod API. It is an early export only: never register_service, no
 * service name, no capability rows. The canonical archive identity record comes from
 * kiln_eml_archive.h and is never redeclared here.
 *
 * Discovery: kiln-sfi resolves KILN_EML_STARTUP_SERVICE_EXPORT from the already-loaded
 * kiln-eml module in its early phase (kiln-eml's kiln_plugin_early has run first). The
 * getter is any-thread and side-effect free and returns one process-lifetime immutable
 * table (v1: header.size 56, header.version 1): min_version<=1 -> the table, >1 -> NULL.
 * A missing export means an older kiln-eml (no seam). The getter grants nothing; every
 * member checks the key. The table is append-only: published_identity was appended to
 * the first five members (48 -> 56 bytes, still version 1); a kiln-eml whose table is
 * 48 bytes has none, so a caller checks KILN_EML_STARTUP_HAS before it reads a member.
 *
 * Key: launch_id (never 0) and plan_sha256 (raw bytes) are kiln.launch's launch id and
 * plan hash as kiln-sfi read them; kiln-eml compares them with its own kiln.launch read.
 * flags is zero in v1. session is 0 for attach and published_identity and the value attach
 * issued for every other member. In-process code is trusted: the session prevents
 * accidental misuse; it is not access control.
 *
 * Record rules (as kiln.eml.archive): size is caller-set, at least the v1 size (key 56,
 * witness 40, identity 160), also on the witness and identity a member fills; larger
 * sizes are accepted and bytes past v1 are untouched. A filled record gets its v1 fields
 * only (flags and reserved zero, size kept). Nonzero v1 flags/reserved in an input
 * record, a short size, a null pointer where one is needed, or a key that does not
 * match is INVALID_ARGUMENT. Pointers are not retained after return and
 * input/output buffers must not overlap. Null text is allowed only with zero capacity.
 * required is mandatory. Text is counted UTF-8 WITHOUT a NUL. On BUFFER_TOO_SMALL
 * required is the COMPLETE byte count and nothing else is written (not the witness, not
 * the identity); on OK required is the complete count. Other failures leave every output
 * unchanged. No partial writes or truncation. A call refused for its key or its records
 * changes no state: it is not the one attach and not the one terminal.
 *
 * attach(key, planned_dlls, session): once per process, from kiln-sfi's early phase,
 * before its loader is armed. planned_dlls is the number of DLLs in kiln-sfi's native
 * plan (0 is valid); the terminal must list exactly that many. OK issues the nonzero
 * session. Only an OK attach counts: any later attach is UNAVAILABLE, and so is an attach
 * in a start that is not a kiln-backend start, after kiln-eml's early phase failed, or
 * once kiln-eml's startup barrier has begun (kiln-eml then runs without native facts).
 *
 * witness_wait(key, timeout_ms, out, reason): blocks at most timeout_ms (0 polls). OK
 * with out->state READY (out->plan_id is the launch plan's plan_id, out->revision
 * kiln-eml's opaque witness revision; required 0), REFUSED (reason holds why, required
 * > 0) or PENDING (the timeout passed, required 0). plan_id and revision are zero unless
 * READY. The state only moves PENDING -> READY or PENDING -> REFUSED and then never
 * changes; calls may repeat, so after BUFFER_TOO_SMALL a call with timeout 0 returns the
 * same answer. Only READY, for kiln-sfi's own plan_id, lets a package DLL load.
 *
 * witness_recheck(key, w, reason): once every planned DLL is terminal, before
 * native_terminal (the answer is the terminal's coherent field). w must equal the READY
 * witness this session received (state, plan_id, revision); anything else, or no READY
 * witness, is INVALID_ARGUMENT. OK with required 0: the witnessed input is unchanged. OK
 * with required > 0: it is not, and reason holds why (BUFFER_TOO_SMALL also means not
 * unchanged). UNAVAILABLE once kiln-eml's wait for the terminal has ended (a terminal was
 * accepted or the wait expired). kiln-eml's own check before its pass stays the authority.
 *
 * native_terminal(key, json): the native terminal, a native-terminal.schema.json v1
 * record (counted UTF-8). The first call with a valid key counts, whatever its JSON.
 * kiln-eml refuses (INVALID_ARGUMENT) a record that fails the schema, whose planId is not
 * the plan's, whose dlls are not planned_dlls in number, or that names a package the plan
 * does not list; it then has no terminal (the DLL packages and their dependents are
 * excluded, native-terminal-missing). A later call, or one after kiln-eml's wait
 * expired, is UNAVAILABLE and only logged. OK means kiln-eml's pass uses this terminal;
 * any other result means it does not.
 *
 * outcome(key, identity, json): UNAVAILABLE until kiln-eml's startup barrier has
 * finished; a barrier that faults before its outcome exists leaves it UNAVAILABLE for
 * the process. Afterwards every call gives the same answer: OK (or BUFFER_TOO_SMALL with
 * the complete size) with the same bytes, a startup-outcome.schema.json v1 record, and
 * the same identity. identity->phase is KILN_EML_ARCHIVE_PUBLISHED exactly when the
 * outcome's grant is true, and the identity then holds the published generation's pins:
 * launch_id = 8 zero bytes then the key's launch_id big-endian; plan_id = the plan's;
 * plan_sha256 = the key's; generation nonzero; reload_epoch 1; archive_sha256 = the
 * JSON's archiveSha256; registry_id/registry_sha256 = the generation's registry.
 * Otherwise every v1 field after size is zero. The JSON's launchId, planId and
 * planSha256 name the same launch. kiln-sfi cross-checks phase, launch_id, plan_id and
 * plan_sha256 (and the JSON against the identity) before it acquires a view
 * (kiln.eml.archive acquire); a mismatch is startup-grant-mismatch and no grant.
 *
 * published_identity(key, identity): the identity alone of the generation kiln-eml
 * granted this launch, for a reader that is not kiln-sfi (kiln.boot's OnBoot game data
 * and kiln.content's resource-patch check in a kiln-backend start). Session-free (key.session 0) and read-only: it changes no
 * state, attaches nothing and grants nothing. OK, with *identity exactly the identity
 * outcome hands (PUBLISHED, the published generation's pins; size kept), only once
 * kiln-eml's startup barrier has finished with a grant; UNAVAILABLE before that, for a
 * start without a grant (the outcome's grant false, or a barrier that faulted), and in a
 * process with no kiln-backend start of this kiln-eml (no launch installed); a key that
 * does not match the installed launch (launch_id, plan_sha256), a nonzero session, a short
 * record or a null pointer is INVALID_ARGUMENT. Nothing is written on a failure. The
 * caller acquires the view itself (kiln.eml.archive acquire with exactly these pins).
 *
 * Threads: every member is any-thread, never enters Lua, never calls back into the
 * caller and is panic-contained (a contained fault is INTERNAL). witness_wait blocks at
 * most timeout_ms; no other member blocks on kiln-eml's pass. Loader lock: kiln-sfi may
 * call witness_recheck and native_terminal on its deadline thread while a package's
 * DllMain holds the loader lock, and kiln.boot calls published_identity at its bound()
 * while a timed-out package's DllMain may still hold it, so these members, and anything
 * they log (the core's log included), never load a library, never resolve a symbol lazily (GetProcAddress, a
 * delay-load import's first call) and never start and join a thread; kiln-eml's wait for
 * the terminal is bound by the same rule.
 *
 * JSON payloads are ABI: schema/sfi/eml-startup/ (format 1), fixtures in
 * schema/sfi/eml-startup/fixtures/. Chains are nearest first (the failed package last),
 * as sfi-state's. Writers clamp free text at a character boundary and capped cause lists
 * to their first entries; rows, DLL records and ids are never clamped. See
 * docs/kiln/services/eml-startup.md.
 */
#ifndef KILN_EML_STARTUP_H
#define KILN_EML_STARTUP_H
#include "kiln_eml_archive.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_EML_STARTUP_VERSION 1u
#define KILN_EML_STARTUP_SERVICE_EXPORT "kiln_eml_startup_service"
#define KILN_EML_STARTUP_FORMAT 1u
#define KILN_EML_STARTUP_KEY_SIZE 56u
#define KILN_EML_STARTUP_WITNESS_SIZE 40u

enum { KILN_EML_WITNESS_PENDING = 0, KILN_EML_WITNESS_READY = 1, KILN_EML_WITNESS_REFUSED = 2 };

typedef struct KilnEmlStartupKeyV1 {
    uint32_t size;
    uint32_t flags; /* v1: zero */
    uint64_t launch_id;
    uint8_t plan_sha256[32];
    uint64_t session;
} KilnEmlStartupKeyV1;

/* plan_id: RFC 4122/network order, like KilnEmlArchiveIdentityV1. */
typedef struct KilnEmlStartupWitnessV1 {
    uint32_t size;
    uint32_t state;
    uint8_t plan_id[16];
    uint64_t revision;
    uint32_t flags; /* v1: zero */
    uint32_t reserved;
} KilnEmlStartupWitnessV1;

typedef struct KilnEmlStartupApiV1 {
    KilnServiceHeader header;
    KilnResult(KILN_CALL* attach)(const KilnEmlStartupKeyV1* key, uint32_t planned_dlls, uint64_t* session);
    KilnResult(KILN_CALL* witness_wait)(const KilnEmlStartupKeyV1* key, uint32_t timeout_ms,
                                        KilnEmlStartupWitnessV1* out, char* reason, size_t capacity, size_t* required);
    KilnResult(KILN_CALL* witness_recheck)(const KilnEmlStartupKeyV1* key, const KilnEmlStartupWitnessV1* witness,
                                           char* reason, size_t capacity, size_t* required);
    KilnResult(KILN_CALL* native_terminal)(const KilnEmlStartupKeyV1* key, KilnStr json);
    KilnResult(KILN_CALL* outcome)(const KilnEmlStartupKeyV1* key, KilnEmlArchiveIdentityV1* identity,
                                   char* json, size_t capacity, size_t* required);
    KilnResult(KILN_CALL* published_identity)(const KilnEmlStartupKeyV1* key, KilnEmlArchiveIdentityV1* identity);
} KilnEmlStartupApiV1;

/* The early export (kiln-eml.dll). This declaration does not export or implement it in MIT. */
const KilnEmlStartupApiV1* KILN_CALL kiln_eml_startup_service(uint32_t min_version);
typedef const KilnEmlStartupApiV1*(KILN_CALL* KilnEmlStartupServiceFn)(uint32_t min_version);

/* Short circuit BEFORE reading a tail pointer. api must be a stable pointer expression. */
#define KILN_EML_STARTUP_HAS(api, member) \
    ((api) != NULL && ((const KilnEmlStartupApiV1*)(api))->header.size >= \
         offsetof(KilnEmlStartupApiV1, member) + sizeof(((KilnEmlStartupApiV1*)0)->member) && \
     ((const KilnEmlStartupApiV1*)(api))->header.version >= KILN_EML_STARTUP_VERSION && \
     ((const KilnEmlStartupApiV1*)(api))->member != NULL)

#ifdef __cplusplus
}
#define KILN_EML_STARTUP_ASSERT(c) static_assert(c, #c)
#define KILN_EML_STARTUP_ALIGN(t) alignof(t)
#else
#define KILN_EML_STARTUP_ASSERT(c) _Static_assert(c, #c)
#define KILN_EML_STARTUP_ALIGN(t) _Alignof(t)
#endif
#define KILN_EML_STARTUP_FIELD(t, f, o) KILN_EML_STARTUP_ASSERT(offsetof(t, f) == o)
#define KILN_EML_STARTUP_LAYOUT(t, s, a) \
    KILN_EML_STARTUP_ASSERT(sizeof(t) == s); KILN_EML_STARTUP_ASSERT(KILN_EML_STARTUP_ALIGN(t) == a)
#if UINTPTR_MAX == UINT64_MAX
KILN_EML_STARTUP_LAYOUT(KilnEmlStartupKeyV1, KILN_EML_STARTUP_KEY_SIZE, 8);
KILN_EML_STARTUP_FIELD(KilnEmlStartupKeyV1, size, 0);
KILN_EML_STARTUP_FIELD(KilnEmlStartupKeyV1, flags, 4);
KILN_EML_STARTUP_FIELD(KilnEmlStartupKeyV1, launch_id, 8);
KILN_EML_STARTUP_FIELD(KilnEmlStartupKeyV1, plan_sha256, 16);
KILN_EML_STARTUP_FIELD(KilnEmlStartupKeyV1, session, 48);
KILN_EML_STARTUP_LAYOUT(KilnEmlStartupWitnessV1, KILN_EML_STARTUP_WITNESS_SIZE, 8);
KILN_EML_STARTUP_FIELD(KilnEmlStartupWitnessV1, size, 0);
KILN_EML_STARTUP_FIELD(KilnEmlStartupWitnessV1, state, 4);
KILN_EML_STARTUP_FIELD(KilnEmlStartupWitnessV1, plan_id, 8);
KILN_EML_STARTUP_FIELD(KilnEmlStartupWitnessV1, revision, 24);
KILN_EML_STARTUP_FIELD(KilnEmlStartupWitnessV1, flags, 32);
KILN_EML_STARTUP_FIELD(KilnEmlStartupWitnessV1, reserved, 36);
KILN_EML_STARTUP_LAYOUT(KilnEmlStartupApiV1, 56, 8);
KILN_EML_STARTUP_FIELD(KilnEmlStartupApiV1, header, 0);
KILN_EML_STARTUP_FIELD(KilnEmlStartupApiV1, attach, 8);
KILN_EML_STARTUP_FIELD(KilnEmlStartupApiV1, witness_wait, 16);
KILN_EML_STARTUP_FIELD(KilnEmlStartupApiV1, witness_recheck, 24);
KILN_EML_STARTUP_FIELD(KilnEmlStartupApiV1, native_terminal, 32);
KILN_EML_STARTUP_FIELD(KilnEmlStartupApiV1, outcome, 40);
KILN_EML_STARTUP_FIELD(KilnEmlStartupApiV1, published_identity, 48);
KILN_EML_STARTUP_LAYOUT(KilnEmlArchiveIdentityV1, 160, 8);
#endif
#undef KILN_EML_STARTUP_LAYOUT
#undef KILN_EML_STARTUP_FIELD
#undef KILN_EML_STARTUP_ALIGN
#undef KILN_EML_STARTUP_ASSERT
#endif
