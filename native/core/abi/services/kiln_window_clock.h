/* SPDX-License-Identifier: MIT
 * kiln.window_clock v1 (in-core service, server side): where the server world's step is right now.
 *
 * "On the tick thread" is not enough to know that the world may be read: a game program can run on that same thread
 * in the middle of the sim step, while the world's jobs are live. The clock names the part of the step:
 *
 *   1 PRE        the Pre subscribers run (kiln_api subscribe_tick, KILN_TICK_PRE)
 *   2 PREPARING  after the last Pre subscriber: the window services' window_begin calls
 *   3 SIM        the game's own sim step (programs, world jobs; workers when a player is in the world)
 *   4 AFTER_JOBS after WaitWorldJobs: the window services' window_end calls
 *   5 POST       the Post subscribers run
 *   6 STOPPING   the world is being torn down (KILN_WORLD_STOPPING); teardown only
 *   0            no server world, or no step has run in it yet
 *
 * World-safe work (reading or writing components through the services, resolving an entity) needs the world thread
 * AND a phase of 1, 2, 4 or 5. Phase 3 is never safe, also on the world thread.
 *
 * world_epoch counts server worlds of this process: it goes up by one at each KILN_WORLD_STARTED and is never a
 * pointer. step is the sim step counter the tick subscribers get. process_session is 16 random bytes made once per
 * process: a token that carries it (an equipment snapshot, a decision id kept by a mod) is refused by the next
 * process although its numbers may repeat.
 *
 * get() copies one consistent state and may be called on any thread. Only the core writes the clock.
 * The window calls (2 and 4) exist only where feature "tick.wait_jobs" is on; without it the phases 2 and 4 are
 * skipped and the window services report unavailable.
 */
#pragma once
#include "../kiln_api.h"
#pragma pack(push, 8)
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_WINDOW_CLOCK_SERVICE "kiln.window_clock"
#define KILN_WINDOW_CLOCK_VERSION 1

#define KILN_WINDOW_NONE 0u
#define KILN_WINDOW_PRE 1u
#define KILN_WINDOW_PREPARING 2u
#define KILN_WINDOW_SIM 3u
#define KILN_WINDOW_AFTER_JOBS 4u
#define KILN_WINDOW_POST 5u
#define KILN_WINDOW_STOPPING 6u

typedef struct KilnWindowStateV1 {
    uint32_t size, phase;
    uint64_t world_epoch, step;
    uint8_t process_session[16];
    uint32_t world_thread, reserved; /* OS thread id of the world thread, 0 before the first step */
} KilnWindowStateV1; /* 48 */

typedef struct KilnWindowClockApiV1 {
    KilnServiceHeader header;
    /* Fills *out (out->size is set by the callee). KILN_ERR_INVALID_ARG for null. Any thread. */
    KilnResult(KILN_CALL* get)(KilnWindowStateV1* out);
} KilnWindowClockApiV1;

/* True when a state allows world-safe work on the calling thread `thread_id` (GetCurrentThreadId). */
static inline int kiln_window_world_safe(const KilnWindowStateV1* s, uint32_t thread_id) {
    return s->world_thread != 0 && s->world_thread == thread_id &&
           (s->phase == KILN_WINDOW_PRE || s->phase == KILN_WINDOW_PREPARING || s->phase == KILN_WINDOW_AFTER_JOBS ||
            s->phase == KILN_WINDOW_POST);
}

#ifdef __cplusplus
}
static_assert(sizeof(KilnWindowStateV1) == 48, "KilnWindowStateV1 is 48 bytes");
#endif
#pragma pack(pop)
