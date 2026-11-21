// SPDX-License-Identifier: MIT
// The content worker's lifetime guard (Kiln-internal): ShroudForge's ResourceLifeGuard over the
// caller's record (kiln_engine_view.h ResourceGuard). Any thread except, for an owned acquire, a world's own; no
// allocation, no lock, no wait, no KilnApi call, no log (tests/lint_gate.py checks this file and resource_guard.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

#include "gate.h"  // CallerThread, GateState, gate_validate

namespace kiln::engine {

// TryAcquire. `gate` non-null: borrow that held gate's registry share (no word taken, `caller` unused: the gate already
// proved its thread, and the caller is inside its hold). `gate` null: refuse a world thread (`caller`, from the core),
// resolve the owners, then try the registry word Shared. `layout` NULL: no exact profile. On any refusal `guard` is left
// released (its size kept). The caller checked the arguments (guard size, service and reader nonzero, gate size).
Reason guard_acquire(const Layout* layout, const GateState* gate, std::uintptr_t expected_service,
                     std::uintptr_t expected_reader, ResourceGuard& guard, CallerThread caller, std::uintptr_t image);
bool guard_validate(const Layout* layout, const ResourceGuard& guard);
bool guard_contains(const ResourceGuard& guard, std::uintptr_t address, std::size_t size);
void guard_release(ResourceGuard& guard) noexcept;

#ifdef KILN_ENGINE_GATE_TEST_POINTS
// Host tests only: called inside guard_acquire with the registry word held or borrowed, before the registry scan (Held)
// and after it, before the final validation (Scanned), to change the fake engine at an exact point.
enum class GuardPoint : std::uint32_t { Held, Scanned };
extern void (*g_guard_test_point)(GuardPoint);
#endif

}  // namespace kiln::engine
