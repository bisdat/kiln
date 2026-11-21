/* SPDX-License-Identifier: MIT
 * Kiln-internal (NOT core/abi, NOT kiln-abi, NOT the SDK): checked with the build stamp, never an ABI promise.
 *
 * kiln.internal.hooks v1: code ranges a Kiln service protects for its engine calls (the recipes of kiln.internal.engine's
 * ecs_gate_invoke, docs/kiln/services/engine-call-recipes.md). Once a range is protected, kiln.hooks refuses every
 * install whose detour would write over it (all link kinds), and every patch and write that overlaps it, with
 * KILN_ERR_UNAVAILABLE and a kiln.log line naming the protecting owner and its reason (one owner's refusals over one
 * range are logged the 1st, 2nd, 4th, 8th, ... time). Adjacent bytes stay hookable.
 *  - resolved only through the services' KilnApi (the core's kiln.internal. name rule, api.cpp). The plugin, managed
 *    host and public views never resolve it, and nobody publishes the name, not even a Kiln service;
 *  - protect_code_ranges(owner, ranges, n, reason): every range must lie inside the exe's .text (the ranges
 *    kiln.hooks read_original and code_state accept), 1 <= n <= KILN_HOOKS_PROTECT_MAX_PER_CALL, the ranges together at
 *    most KILN_HOOKS_CODE_RANGE_MAX bytes, reason 1..KILN_HOOKS_PROTECT_REASON_MAX bytes, owner a live owner other
 *    than 0. Under kiln.hooks' code lock (the one its writers and code_state hold) every range must read
 *    KILN_CODE_ORIGINAL with no Kiln detour or patch recorded over it (a patch that put the file's bytes back over
 *    another still counts: undoing it would change the code); then all of them are protected together, or none is.
 *    So an install racing it either lands first (this call answers KILN_ERR_UNAVAILABLE, nothing protected: the
 *    engine call stays unavailable) or is refused, never both. Calling it again with ranges the owner already
 *    protects is KILN_OK and adds nothing (the ORIGINAL check runs again);
 *  - results: KILN_OK; KILN_ERR_INVALID_ARGUMENT (the rules above); KILN_ERR_REVOKED (the owner is not alive);
 *    KILN_ERR_UNAVAILABLE (a range is not ORIGINAL now, the exe file's code is not available, or the process-wide
 *    bounds KILN_HOOKS_PROTECT_MAX_RANGES / KILN_HOOKS_PROTECT_MAX_BYTES would be passed);
 *  - a protection lasts until its owner retires (the core's owner-retiring notification); nothing else removes it;
 *  - it binds kiln.hooks' writers only: another module, or Kiln code that writes game code without kiln.hooks, can still
 *    write the bytes, so the engine call's own compare of the live code with its capture stays;
 *  - any thread except inside a gate's hold (it takes kiln.hooks' code lock and may log); never call it under the
 *    loader lock (from DllMain or a TLS callback). It never stops a thread and adds no wait to an install beyond the
 *    code lock the install already takes.
 * With no protection registered, kiln.hooks behaves exactly as without this table.
 */
#pragma once
#include <cstddef>
#include <cstdint>

#include "kiln_api.h"

#define KILN_HOOKS_INTERNAL_SERVICE "kiln.internal.hooks"
#define KILN_HOOKS_INTERNAL_VERSION 1u

#define KILN_HOOKS_PROTECT_MAX_PER_CALL 32u                /* a recipe's ranges (kiln_engine_layout.h kCallRanges) */
#define KILN_HOOKS_PROTECT_MAX_RANGES 256u                 /* every owner's ranges together */
#define KILN_HOOKS_PROTECT_MAX_BYTES (512u * 1024u)        /* their sizes together (8 recipes of 64 KiB) */
#define KILN_HOOKS_PROTECT_REASON_MAX 256u

extern "C" {

typedef struct KilnCodeRange {
    uintptr_t address; /* live address inside the exe's .text */
    size_t size;       /* bytes, at least 1 */
} KilnCodeRange;

typedef struct KilnHooksInternalApi {
    KilnServiceHeader header; /* { sizeof(KilnHooksInternalApi), KILN_HOOKS_INTERNAL_VERSION } */
    KilnResult(KILN_CALL* protect_code_ranges)(uint64_t owner, const KilnCodeRange* ranges, size_t n, KilnStr reason);
} KilnHooksInternalApi;

}  // extern "C"

static_assert(sizeof(KilnCodeRange) == 16 && offsetof(KilnCodeRange, size) == 8, "KilnCodeRange");
static_assert(sizeof(KilnHooksInternalApi) == 16 && offsetof(KilnHooksInternalApi, protect_code_ranges) == 8,
              "kiln.internal.hooks v1 (Kiln-internal; checked with the build stamp, not an ABI promise)");
