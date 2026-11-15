// SPDX-License-Identifier: MIT
// kiln.window_clock for the tick hook (services/window_clock.cpp). The state machine is window_clock_core.h.
#pragma once
#include <cstdint>

namespace kiln::window_clock {

// Whether the window calls (phases 2 and 4) run at all: the tick hook has WaitWorldJobs.
void enable_windows(bool on);
// True when a window service was begun in this step and the windows are enabled: the step then waits for the
// world's jobs and calls end(), also with no Post subscriber. A service that wanted nothing this step does not count.
bool wants_end();

void world_started();            // KILN_WORLD_STARTED (server), before its subscribers
void pre(uint64_t step);         // before the Pre subscribers
void begin();                    // after the last Pre subscriber (also when Pre failed: the phase must reach Sim)
void end();                      // after WaitWorldJobs, before the Post subscribers
void stopping();                 // KILN_WORLD_STOPPING (server), before its subscribers
void stopped();                  // after them

}  // namespace kiln::window_clock
