/* SPDX-License-Identifier: MIT
 * Kiln-internal (NOT core/abi, NOT kiln-abi, NOT the SDK): checked with the build stamp, never an ABI promise.
 *
 * kiln.internal.owners v1: an owner's id, as the core keeps it (the id owner_create was given: "mod:<manifest id>" for a
 * Kiln mod's owner from Kiln.Host, a native mod's or a plugin's own id). kiln.registry (kiln-content.dll) binds a caller to
 * its mod with it (kiln_registry.h "Mods": the id without its kind prefix, folded to lower case).
 *  - resolved only through the services' KilnApi (the core's kiln.internal. name rule, api.cpp): a kiln/services DLL of
 *    this very build. The plugin, managed host and public views never resolve it, and nobody publishes the name;
 *  - owner_id(owner, buf, cap, out_len): the id copied into buf, NUL-terminated, cut at cap - 1 bytes; *out_len (may be
 *    NULL) = the id's length without the NUL. KILN_OK; KILN_ERR_BUFFER_TOO_SMALL (cap < *out_len + 1); KILN_ERR_NOT_FOUND
 *    (the owner is not alive: never created, or revoked); KILN_ERR_INVALID_ARGUMENT (owner 0, the core; buf NULL with
 *    cap > 0). Any thread.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "kiln_api.h"

#define KILN_OWNERS_INTERNAL_SERVICE "kiln.internal.owners"
#define KILN_OWNERS_INTERNAL_VERSION 1u

typedef struct KilnOwnersInternalApi {
    KilnServiceHeader header; /* { sizeof(KilnOwnersInternalApi), KILN_OWNERS_INTERNAL_VERSION } */
    KilnResult(KILN_CALL* owner_id)(KilnOwner owner, char* buf, size_t cap, size_t* out_len);
} KilnOwnersInternalApi;
