/* SPDX-License-Identifier: MIT
 * Kiln-internal (NOT core/abi, NOT kiln-abi, NOT the SDK): checked with the build stamp, never an ABI promise.
 *
 * kiln.internal.owners v1: what the managed host admitted an owner to, for Kiln's own services (kiln.net's steam_open
 * admits an owner with KILN_OWNER_GRANT_STEAM_MESSAGES). The grants live in the core's owner registry, next to the owner.
 *  - resolved only through the services' KilnApi and the managed host's KilnApi (the core's kiln.internal. name rule,
 *    api.cpp). The plugin and public views never resolve it, and nobody publishes the name, not even a Kiln service;
 *  - grant(owner, grants): the managed host marks an owner it made for a mod with what that mod's kiln.mod.json opts
 *    into (KILN_OWNER_GRANT_*, OR'ed into what the owner holds). Grants are never taken back: they go with the owner
 *    when it is revoked, and a reloaded mod's new owner starts with none. KILN_ERR_INVALID_ARGUMENT: owner 0, no bit,
 *    or a bit this core does not know; KILN_ERR_REVOKED: the owner is not alive (revoked, or never made);
 *  - grants(owner, out): a live owner's grants (0 when it holds none). KILN_ERR_REVOKED: the owner is not alive
 *    (revoked, or never made); KILN_ERR_INVALID_ARGUMENT: owner 0 or out NULL.
 * Any thread; each call takes the owner registry's lock for a lookup and never calls out.
 */
#pragma once
#include <cstddef>
#include <cstdint>

#include "kiln_api.h"

#define KILN_OWNERS_INTERNAL_SERVICE "kiln.internal.owners"
#define KILN_OWNERS_INTERNAL_VERSION 1u

extern "C" {

enum {
    KILN_OWNER_GRANT_STEAM_MESSAGES = 1, /* kiln.mod.json "steamMessages": true: kiln.net's steam_open admits the owner */
    KILN_OWNER_GRANTS_KNOWN = 1,         /* every bit this core knows */
};

typedef struct KilnOwnersInternalApi {
    KilnServiceHeader header; /* { sizeof(KilnOwnersInternalApi), KILN_OWNERS_INTERNAL_VERSION } */
    KilnResult(KILN_CALL* grant)(KilnOwner owner, uint32_t grants);
    KilnResult(KILN_CALL* grants)(KilnOwner owner, uint32_t* out);
} KilnOwnersInternalApi;

}  // extern "C"

static_assert(sizeof(KilnOwnersInternalApi) == 24 && offsetof(KilnOwnersInternalApi, grant) == 8 &&
                  offsetof(KilnOwnersInternalApi, grants) == 16,
              "kiln.internal.owners v1 (Kiln-internal; checked with the build stamp, not an ABI promise)");
