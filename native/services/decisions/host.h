// SPDX-License-Identifier: MIT
// kiln.decisions: what the service (decisions.cpp) gives its producers (death.cpp, ...) and what each producer gives
// back. Internal to kiln-decisions.dll.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "kiln_api.h"
#include "metadata.h"
#include "queue.h"
#include "services/kiln_caps.h"
#include "services/kiln_decisions.h"
#include "services/kiln_hooks.h"
#include "services/kiln_world.h"

namespace kiln::decisions::seh {
// shutdown's guard around a hook body (seh.cpp): false when it faulted or threw, with the exception code.
bool call(void (*body)(KilnRegisters& r), KilnRegisters& r, unsigned long* code);
}  // namespace kiln::decisions::seh

namespace kiln::decisions::host {

const KilnApi* api();
KilnOwner owner();
const KilnWorldApi* world();
const KilnCapsApi* caps();
Broker& broker();

// Whether `ctx` (a hooked program's ProgramContext) is an invocation of the core's server world (world_gate.h). Any
// thread; false for a hosting client's client and prediction worlds and wherever there is no server world.
bool in_server_world(const void* ctx);
// Whether the world layout constants are known (else no hook can tell the server world from another).
bool world_layout_known();

// The window the step is in: set at window_begin (any thread may read them).
uint64_t epoch();
uint64_t step();
// The world thread's OS id (the thread of the last window_begin).
uint32_t world_thread();

// The consumer's interest in a kind: 0 none, 1 a list of entities, 2 every entity. Any thread, lock free.
uint32_t interest_state(uint32_t kind);
// Whether the interest in `kind` names `entity` (any thread; takes the interest lock for a list).
bool interested(uint32_t kind, uint32_t entity);
// Whether a producer may hold now: the metadata fits its budget and the outcomes are read. Any thread.
bool holding();
// The metadata table of this window (never null). Any thread.
std::shared_ptr<const Table> table();
// One row sampled now. World thread, world-safe phase only (the caller is a finisher or a window call).
bool resolve(uint32_t entity, Row* out);

// A producer can hold its kind (resolved; its hook goes in at the first listener): the kind is reported in
// supported_kinds. hook_kind_off: its hook could not go in after all.
void hook_kind(uint32_t kind);
void hook_kind_off(uint32_t kind);
// The service holds at all (not shut down after a fault). Any thread, lock free: the hooks ask it first.
bool holding_allowed();
// A hook's body faulted (`what` names the hook): the service stops holding now and shuts down at its next world-thread
// call (shutdown.h). Any thread.
void hook_faulted(const char* what, unsigned long code);
// Lowers a capability row (unavailable, `reason`, `why`). Any thread.
void lower(const char* row, uint32_t reason, const char* why);
void logf(int level, const char* fmt, ...);

}  // namespace kiln::decisions::host

// The death producer (death.cpp).
namespace kiln::decisions::death_producer {

// The service is on (on_ready): resolves what the hook needs (nothing is installed: events.death is ready unless held
// or unresolved). arm(): puts the hook in now (a mod listens, a native guard is set); false: it cannot (the rows are
// lowered with the reason when the install itself failed). Any thread.
void ready();
bool arm();
bool installed();
bool usable();  // ready or armed
// Something to keep the windows for: a hold, or a native guard (it asks the window's epoch).
bool busy();
// The shutdown: forgets every hold unless a fault left the lock held (false then).
bool try_clear();
// window_begin, before the metadata is rebuilt: the entities the death producer needs rows for (every entity with
// Health while a listener takes all deaths). World thread, no world job live.
void discover(std::vector<uint32_t>& wanted);
// window_begin, after the broker's deadlines: releases holds nobody listens for, forgets old ones.
void window_begin(uint64_t epoch, uint64_t step);
void world_stopped();
// KilnDecisionsApiV1.set_death_guard. Any thread.
KilnResult set_guard(KilnOwner owner, KilnDeathGuardFn guard);
// The words for decisions.native_guards and events.death when the producer cannot hold (empty when it can).
std::string why_not();
uint32_t why_not_reason();

}  // namespace kiln::decisions::death_producer

// The walk-over pickup producer (pickup.cpp).
namespace kiln::decisions::pickup_producer {

// The service is on (on_ready): resolves what the hook needs (nothing installed). arm(): puts it in now (a mod
// listens); false: it cannot (events.pickup is lowered with the reason when the install itself failed). Any thread.
void ready();
bool arm();
bool installed();
bool usable();  // ready or armed
bool busy();    // an exposure is sampled
bool try_clear();  // the shutdown: forgets every exposure unless a fault left the lock held (false then)
// The words and reason for events.pickup when the producer cannot hold (empty / KILN_CAP_REASON_NONE when it can).
std::string why_not();
uint32_t why_not_reason();
// window_begin, before the metadata is rebuilt: drops (PickupItemZone) and live players while anyone listens.
void discover(std::vector<uint32_t>& wanted);
// Both window calls: the exposure samples (window_begin also checks the keyed lives).
void window_begin(uint64_t epoch, uint64_t step);
void window_end(uint64_t epoch, uint64_t step);
void world_stopped();
// KilnDecisionsApiV1.reevaluate_pickup (the caller checked thread and phase).
KilnResult reevaluate(uint32_t drop, uint32_t receiver, uint32_t* reset);

}  // namespace kiln::decisions::pickup_producer
