/* SPDX-License-Identifier: MIT
 * Kiln-internal (NOT core/abi, NOT kiln-abi, NOT the SDK): checked with the build stamp, never an ABI promise.
 *
 * kiln.internal.launch v1: the ShroudForge-dialect owner, for Kiln's own services (kiln.engine's gate admission).
 * Loaders read the frozen backend through kiln.launch (abi/services/kiln_launch.h) instead.
 *  - resolved only through the services' KilnApi (the core's kiln.internal. name rule, api.cpp). The plugin, managed
 *    host and public views never resolve it, and nobody publishes the name, not even a Kiln service;
 *  - dialect_owner(): the owner the core passed to kiln_plugin_main of <game>/kiln/plugins/kiln-sfi.dll, or 0. The core
 *    designates at most one owner per process, and only when all of these hold: kiln.launch froze backend kiln; the
 *    DLL is that file (not a copy elsewhere, not another name); its kiln_build_stamp() is this core's build identity
 *    (the test api.cpp's grant_internal uses). It is set before that kiln_plugin_main runs, so before the first tick,
 *    and never changes or moves to another owner afterwards, also when the plugin's main fails;
 *  - no member sets it and no flag opts out of it.
 * Any thread; never blocks.
 */
#pragma once
#include <cstddef>
#include <cstdint>

#include "kiln_api.h"

#define KILN_LAUNCH_INTERNAL_SERVICE "kiln.internal.launch"
#define KILN_LAUNCH_INTERNAL_VERSION 1u

extern "C" {

typedef struct KilnLaunchInternalApi {
    KilnServiceHeader header;                  /* { sizeof(KilnLaunchInternalApi), KILN_LAUNCH_INTERNAL_VERSION } */
    KilnOwner(KILN_CALL* dialect_owner)(void); /* the designated owner, 0 when there is none */
} KilnLaunchInternalApi;

}  // extern "C"

static_assert(sizeof(KilnLaunchInternalApi) == 16 && offsetof(KilnLaunchInternalApi, dialect_owner) == 8,
              "kiln.internal.launch v1 (Kiln-internal; checked with the build stamp, not an ABI promise)");
