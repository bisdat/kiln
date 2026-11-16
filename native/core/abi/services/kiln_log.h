/* SPDX-License-Identifier: MIT
 * kiln.log v1 (in-core service, since 0.2.0-dev.29.3): when a game log line was logged.
 *
 * KilnApi.subscribe_log hands the game's log lines to subscribers on Kiln's log thread, in batches: a line can reach a
 * subscriber well after the game logged it, when the log thread was busy writing kiln.log (on a hard drive, seconds).
 * line_time() tells a subscriber, from inside its callback, when the game logged the line being delivered, so a
 * deadline taken from a line (kiln.saves: the game's shutdown gives its last save 10 s from "Trigger gameflow shutdown")
 * starts when the game said it, not when the line arrived.
 *
 * Units: std::chrono::steady_clock nanoseconds since its epoch (QueryPerformanceCounter on Windows), the same clock
 * every Kiln DLL's steady_clock reads. 0 outside a log subscriber's callback, and for a line the core did not capture.
 */
#ifndef KILN_LOG_H
#define KILN_LOG_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_LOG_SERVICE "kiln.log"
#define KILN_LOG_VERSION 1u

typedef struct KilnLogApi {
    KilnServiceHeader header; /* { sizeof(KilnLogApi), KILN_LOG_VERSION } */
    /* In a KilnLogLineFn callback, on Kiln's log thread: when the game logged the line being delivered (see above). */
    int64_t(KILN_CALL* line_time)(void);
} KilnLogApi;

#ifdef __cplusplus
}
#endif
#endif
