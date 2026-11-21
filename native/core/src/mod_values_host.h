/* SPDX-License-Identifier: MIT
 * Kiln-internal (NOT core/abi, NOT kiln-abi, NOT the SDK): the same build's core and Kiln.ModValues only, never an ABI
 * promise. The mods' table is kiln.mod-values v1 (abi/services/kiln_mod_values.h).
 *
 * kiln.internal.mod-values.backend v1: Kiln.ModValues hands the core its process-lifetime backend functions once
 * (bind), and delivers the ledger's events to native subscribers (dispatch). Like kiln.internal.tasks.host (tasks_host.h):
 *  - resolved only through the KilnApi view api_for_managed_host() (api.cpp api_service_managed_host); it is no in-core
 *    service (svc::find never has it), so the plugins', the services' and the public views never resolve it, and nobody
 *    may publish the name (api.cpp register_published refuses it, as every kiln.internal. name for non-services). NULL
 *    while kiln.mod-values is not running (its init did not run or failed);
 *  - exact: Kiln.ModValues takes the table only with header.version == KILN_MOD_VALUES_BACKEND_VERSION and header.size ==
 *    sizeof(KilnModValuesBackendApi), and bind takes only backend->size == sizeof(KilnModValuesBackend). A layout change
 *    is a new version, not an appended member: no function table crosses builds;
 *  - not a sandbox: native code in the process can reach anything.
 *
 * The members:
 *  - bind: the core copies *backend (every function set). The first bind wins for the life of the process; a second
 *    one is INVALID_ARGUMENT (logged). Any thread.
 *  - The core calls the backend on the facade caller's thread, after its own checks, with the owner's validated
 *    namespace as owner_ns, records of Kiln's sizeof, and out zeroed with size set. describe and define also get the
 *    calling owner's handle: the backend registers the definition with it as its token (Registry: a boxed ulong,
 *    compared with Equals), so a revoke retires exactly what that owner made. change gets expected only with
 *    KMV_EXPECT_REVISION, else NULL. The backend fills ok, reason, character, flags, before and after; the core keeps
 *    the public rules (refused: flags 0; read: AFTER; accepted change: BEFORE|AFTER, else KILN_ERR_INTERNAL). A backend
 *    function never lets an exception out: it returns a KilnResult as the public header says.
 *  - retire(owner_ns, owner): an owner that described or defined in owner_ns was revoked: the definitions registered
 *    with its token leave the described and active views (another owner's in the same namespace stay), their data is
 *    parked. On the revoking thread, which need not be the tick thread.
 *  - wanted(): KMV_WANT_* bits of the scopes with a native subscriber now (cheap, lock-free; any thread): the host
 *    builds and dispatches an event only when its bit is set.
 *  - dispatch(change, exported): one event to every matching subscriber, on the calling thread (canonical: server
 *    tick; synced: client main), synchronously. exported is the value's read and sync policy now (rechecked here for
 *    each subscriber of another namespace). INVALID_ARGUMENT for a bad record.
 *
 * The managed mirror is managed/services/Kiln.ModValues/NativeBackend.cs (ModValuesAbi); Kiln.ModValues.Tests'
 * NativeBackendTests read the static asserts below and compare every size and offset with it.
 */
#pragma once
#include <cstddef>
#include <cstdint>

#include "kiln_api.h"
#include "services/kiln_mod_values.h"

#define KILN_MOD_VALUES_BACKEND_SERVICE "kiln.internal.mod-values.backend"
#define KILN_MOD_VALUES_BACKEND_VERSION 1u

extern "C" {

enum { KMV_WANT_CANONICAL = 1u << KMV_CANONICAL_EVENTS, KMV_WANT_SYNCED = 1u << KMV_SYNCED_EVENTS };

typedef struct KilnModValueExport {
    uint32_t size;           /* sizeof(KilnModValueExport) (24) */
    uint32_t read_policy;    /* KilnModReadPolicy */
    uint32_t sync_policy;    /* KilnModSyncPolicy */
    uint32_t read_mod_count; /* KMV_READ_LIST: entries in read_mods */
    const KilnStr* read_mods;
} KilnModValueExport; /* 24 bytes */

typedef struct KilnModValuesBackend {
    uint32_t size; /* exactly sizeof(KilnModValuesBackend) (56) */
    uint32_t reserved;
    KilnResult(KILN_CALL* describe)(KilnStr owner_ns, KilnOwner owner, const KilnModValueDef* def,
                                    KilnModDefinitionResult* out);
    KilnResult(KILN_CALL* define)(KilnStr owner_ns, KilnOwner owner, const KilnModValueDef* def,
                                  KilnModDefinitionResult* out);
    KilnResult(KILN_CALL* read)(KilnStr owner_ns, const KilnModCharacter* character, KilnStr qualified_key,
                                KilnModValueResult* out);
    KilnResult(KILN_CALL* change)(KilnStr owner_ns, const KilnModCharacter* character, KilnStr qualified_key,
                                  uint32_t operation, int64_t amount, const KilnModValueVersion* expected,
                                  uint64_t request_id, KilnModValueResult* out);
    KilnResult(KILN_CALL* read_synced)(KilnStr owner_ns, KilnStr qualified_key, KilnModValueResult* out);
    void(KILN_CALL* retire)(KilnStr owner_ns, KilnOwner owner);
} KilnModValuesBackend; /* 56 bytes */

typedef struct KilnModValuesBackendApi {
    KilnServiceHeader header; /* { sizeof(KilnModValuesBackendApi), KILN_MOD_VALUES_BACKEND_VERSION }: exact */
    KilnResult(KILN_CALL* bind)(const KilnModValuesBackend* backend);
    uint32_t(KILN_CALL* wanted)(void);
    KilnResult(KILN_CALL* dispatch)(const KilnModValueChange* change, const KilnModValueExport* exported);
} KilnModValuesBackendApi; /* 32 bytes */
}

static_assert(sizeof(KilnModValueExport) == 24 && offsetof(KilnModValueExport, read_mods) == 16, "KilnModValueExport v1 (x64)");
static_assert(sizeof(KilnModValuesBackend) == 56 && offsetof(KilnModValuesBackend, describe) == 8 &&
                  offsetof(KilnModValuesBackend, define) == 16 && offsetof(KilnModValuesBackend, read) == 24 &&
                  offsetof(KilnModValuesBackend, change) == 32 && offsetof(KilnModValuesBackend, read_synced) == 40 &&
                  offsetof(KilnModValuesBackend, retire) == 48,
              "KilnModValuesBackend v1 (x64)");
static_assert(sizeof(KilnModValuesBackendApi) == 32 && offsetof(KilnModValuesBackendApi, bind) == 8 &&
                  offsetof(KilnModValuesBackendApi, wanted) == 16 && offsetof(KilnModValuesBackendApi, dispatch) == 24,
              "kiln.internal.mod-values.backend v1 (Kiln-internal; exact version and size, not an ABI promise)");
