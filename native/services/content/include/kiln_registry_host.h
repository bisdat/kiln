/* SPDX-License-Identifier: MIT
 * kiln.internal.registry v1: what Kiln.Host tells kiln.registry (kiln_registry.h) before the stage. Kiln-internal
 * (docs/kiln/services/README.md "Kiln-internal tables"): NOT a public ABI, never in native/core/abi, kiln-abi or the SDK;
 * it may change between Kiln builds. Published by kiln-content.dll (a kiln/services DLL of this very build) and meant for
 * one consumer, the managed host: Kiln.Host checks header.version == KILN_REGISTRY_HOST_VERSION and header.size ==
 * sizeof(KilnRegistryHostApi) and, on a mismatch, logs it and runs the stage without its order (orderSource "folders").
 * Resolving it from the managed host's KilnApi needs the core to hand out this one published internal name to it
 * (native/core/src/api.cpp, api_service_managed_host), as it does kiln.internal.tasks.host: registry slice S11.
 *
 * Kiln mods' C# stage work (Mod.OnRegistry) needs nothing more here: Kiln.Host subscribes one stage callback per such mod
 * through the public kiln.registry subscribe_stage, with that mod's owner and id, and submits the edits its callbacks make
 * through the public submit; it releases what it held for the stage (the throwaway load contexts) at the START event.
 *
 * Threads: both members before the stage (KILN_REGISTRY_PHASE_NONE), any thread; Kiln.Host calls them from Init, after
 * its resolver ordered the mods. Once the stage began: KILN_ERR_UNAVAILABLE, nothing changes.
 */
#ifndef KILN_REGISTRY_HOST_H
#define KILN_REGISTRY_HOST_H
#include <stddef.h>
#include <stdint.h>

#include "kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_REGISTRY_HOST_SERVICE "kiln.internal.registry"
#define KILN_REGISTRY_HOST_VERSION 1u

/* set_mode: where the stage runs (P1-PLAN.md section 1). */
enum {
    KILN_REGISTRY_MODE_RESOURCES_READY = 0, /* option B, the default: at kiln.assets' resources-ready, over the live bytes */
    KILN_REGISTRY_MODE_ONBOOT = 1           /* option A: with OnBoot, over the committed boot generation's base view */
};

typedef struct KilnRegistryHostApi {
    KilnServiceHeader header; /* { sizeof(KilnRegistryHostApi), KILN_REGISTRY_HOST_VERSION } */
    /* Kiln.Host's runtime load order (the launcher profile's order: required and optional dependencies, loadAfter,
     * loadBefore, ties by id) as a JSON array [{"mod": "<id>", "version": "<version>"}], every loaded Kiln mod once. A
     * second call replaces the first. KILN_ERR_INVALID_ARGUMENT: not such an array, or a mod twice (nothing changes). */
    KilnResult(KILN_CALL* set_order)(KilnStr order_json);
    /* KILN_REGISTRY_MODE_*. KILN_ERR_INVALID_ARGUMENT: another value. */
    KilnResult(KILN_CALL* set_mode)(uint32_t mode);
} KilnRegistryHostApi;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
static_assert(sizeof(KilnRegistryHostApi) == 24 && offsetof(KilnRegistryHostApi, set_order) == 8 &&
                  offsetof(KilnRegistryHostApi, set_mode) == 16,
              "kiln.internal.registry v1 (Kiln-internal; checked with the build stamp, not an ABI promise)");
#else
_Static_assert(sizeof(KilnRegistryHostApi) == 24 && offsetof(KilnRegistryHostApi, set_order) == 8 &&
                   offsetof(KilnRegistryHostApi, set_mode) == 16,
               "kiln.internal.registry v1 (Kiln-internal; checked with the build stamp, not an ABI promise)");
#endif

#endif /* KILN_REGISTRY_HOST_H */
