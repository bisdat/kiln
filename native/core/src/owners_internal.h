/* SPDX-License-Identifier: MIT
 * Kiln-internal (NOT core/abi, NOT kiln-abi, NOT the SDK): checked with the build stamp, never an ABI promise.
 *
 * kiln.internal.owners: what the core knows of an owner, for Kiln's own services and the managed host.
 *  - resolved only through the services' KilnApi and the managed host's KilnApi (the core's kiln.internal. name rule,
 *    api.cpp). The plugin and public views never resolve it, and nobody publishes the name, not even a Kiln service. One
 *    table answers both views and every version up to KILN_OWNERS_INTERNAL_VERSION (a v1 caller reads its two members).
 * v1 (dev.30): what the managed host admitted an owner to (kiln.net's steam_open admits an owner with
 * KILN_OWNER_GRANT_STEAM_MESSAGES). The grants live in the core's owner registry, next to the owner.
 *  - grant(owner, grants): the managed host marks an owner it made for a mod with what that mod's kiln.mod.json opts
 *    into (KILN_OWNER_GRANT_*, OR'ed into what the owner holds). Grants are never taken back: they go with the owner
 *    when it is revoked, and a reloaded mod's new owner starts with none. KILN_ERR_INVALID_ARGUMENT: owner 0, no bit,
 *    or a bit this core does not know; KILN_ERR_REVOKED: the owner is not alive (revoked, or never made);
 *  - grants(owner, out): a live owner's grants (0 when it holds none). KILN_ERR_REVOKED: the owner is not alive
 *    (revoked, or never made); KILN_ERR_INVALID_ARGUMENT: owner 0 or out NULL.
 * v2 (dev.31, appended at offset 24): an owner's id, as the core keeps it (the id owner_create was given: "mod:<manifest
 * id>" for a Kiln mod's owner from Kiln.Host, a native mod's or a plugin's own id). kiln.registry (kiln-content.dll) binds
 * a caller to its mod with it (kiln_registry.h "Mods": the id without its kind prefix, folded to lower case); it resolves
 * the table with min_version 2.
 *  - owner_id(owner, buf, cap, out_len): the id copied into buf, NUL-terminated, cut at cap - 1 bytes; *out_len (may be
 *    NULL) = the id's length without the NUL. KILN_OK; KILN_ERR_BUFFER_TOO_SMALL (cap < *out_len + 1); KILN_ERR_REVOKED
 *    (the owner is not alive: revoked, or never made); KILN_ERR_INVALID_ARGUMENT (owner 0, the core; buf NULL with cap > 0).
 * Any thread; each call takes the owner registry's lock for a lookup and never calls out.
 */
#pragma once
#include <cstddef>
#include <cstdint>

#include "kiln_api.h"

#define KILN_OWNERS_INTERNAL_SERVICE "kiln.internal.owners"
#define KILN_OWNERS_INTERNAL_VERSION 2u

extern "C" {

enum {
    KILN_OWNER_GRANT_STEAM_MESSAGES = 1, /* kiln.mod.json "steamMessages": true: kiln.net's steam_open admits the owner */
    KILN_OWNER_GRANTS_KNOWN = 1,         /* every bit this core knows */
};

typedef struct KilnOwnersInternalApi {
    KilnServiceHeader header; /* { sizeof(KilnOwnersInternalApi), KILN_OWNERS_INTERNAL_VERSION } */
    KilnResult(KILN_CALL* grant)(KilnOwner owner, uint32_t grants);
    KilnResult(KILN_CALL* grants)(KilnOwner owner, uint32_t* out);
    /* ---- v2 ---- */
    KilnResult(KILN_CALL* owner_id)(KilnOwner owner, char* buf, size_t cap, size_t* out_len);
} KilnOwnersInternalApi;

}  // extern "C"

static_assert(offsetof(KilnOwnersInternalApi, grant) == 8 && offsetof(KilnOwnersInternalApi, grants) == 16,
              "kiln.internal.owners v1 (Kiln-internal; checked with the build stamp, not an ABI promise)");
static_assert(offsetof(KilnOwnersInternalApi, owner_id) == 24 && sizeof(KilnOwnersInternalApi) == 32,
              "kiln.internal.owners v2: owner_id appended at 24");
