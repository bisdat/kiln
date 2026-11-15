/* SPDX-License-Identifier: MIT
 * kiln.world.budget v1 (since 0.2.0-dev.29): the room for world programs (chapter 17.4, decision I3), published by
 * kiln-world.dll next to kiln.world. Two budgets in one answer, shapes of kiln_capacity.h:
 *   - kiln_slots (process scope): kiln.world's own quota, 64 programs for Kiln's services, compat loaders and mods
 *     together. kiln_used = programs that are registered or wait for a world with a live callback; reserved = slots
 *     held by programs that cannot run (refused, faulted, skipped by the engine, a declaration no loaded mod bound):
 *     a slot is never freed in a process (the engine keeps every descriptor). game_used 0. EXPANDED: the 64 are installed.
 *   - engine_slots (world scope): the world's own program table, 896 entries. game_used = the game's programs (their
 *     descriptors are in the game's image), kiln_used = the programs Kiln registered in that table, reserved = the rest
 *     (another loader that registers directly). Counted from kiln.world's directory of that world's table, which it
 *     reads in the world's own Pre once the table is final (the program directory of kiln.world v4): a query never
 *     reads game memory.
 * Both are answered for the same world and one revision; readiness says when they cannot be: NO_WORLD (no live world of
 * that kind: a client world on a dedicated server), NOT_READY (the world started and its table is not read yet, the
 * table's layout did not resolve on this build, or this process cannot register programs), ACCOUNTING_MISMATCH (the
 * counts do not add up). Then both clear READY and their counts are 0; access and per_file_max are the configured
 * limits and are always filled. Any thread; lock-free for the caller's purposes (it takes kiln.world's short locks).
 * The table only grows at the end; check header.size. */
#ifndef KILN_WORLD_BUDGET_H
#define KILN_WORLD_BUDGET_H
#include "../kiln_api.h"
#include "kiln_capacity.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_WORLD_BUDGET_SERVICE "kiln.world.budget"
#define KILN_WORLD_BUDGET_VERSION 1u

/* The engine's limits on one program's access lists, as kiln.world checks them (they do not grow with the quota). */
typedef struct KilnProgramAccessLimits {
    uint32_t templates;        /* tpl + tplOptional: 8 */
    uint32_t read_write;       /* read, write and their optional lists together: 42 */
    uint32_t singleton_reads;  /* 64 */
    uint32_t singleton_writes; /* 64 */
    uint32_t other_templates;  /* 9 */
    uint32_t other_accessors;  /* otherRead + otherWrite: 92 */
    uint32_t events_in_out;    /* eventsIn + eventsOut: 24 */
    uint32_t iterator_bytes;   /* 4096 */
} KilnProgramAccessLimits;     /* 32 bytes */

typedef struct KilnProgramBudgetReply {
    uint32_t size;          /* set by the caller: sizeof(KilnProgramBudgetReply) */
    uint32_t readiness;     /* KILN_CAPACITY_VALID when both budgets are ready, else why not (KILN_CAPACITY_*) */
    uint32_t world_kind;    /* the kind asked for */
    uint32_t generation;    /* the core session's counter of that kind for the live world; 0 when there is none */
    uint32_t per_file_max;  /* declarations per kiln.programs.json: 32 */
    uint32_t reserved_zero;
    KilnProgramAccessLimits access; /* +24 */
    KilnCapacityInfo kiln_slots;    /* +56 */
    KilnCapacityInfo engine_slots;  /* +104 */
} KilnProgramBudgetReply;           /* 152 bytes, alignment 8 */

typedef struct KilnWorldBudgetApi {
    KilnServiceHeader header; /* { sizeof(KilnWorldBudgetApi), KILN_WORLD_BUDGET_VERSION } */
    /* world_kind KILN_WORLD_SERVER or KILN_WORLD_CLIENT. KILN_OK for every structurally valid answer, also not ready.
     * KILN_ERR_INVALID_ARGUMENT: another kind (PREDICTION shares the client world's table), out NULL or too small. */
    KilnResult(KILN_CALL* program_budget)(uint32_t world_kind, KilnProgramBudgetReply* out);
} KilnWorldBudgetApi;

#ifdef __cplusplus
}
#endif
#endif /* KILN_WORLD_BUDGET_H */
