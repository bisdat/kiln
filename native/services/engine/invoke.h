// SPDX-License-Identifier: MIT
// One engine call inside a held write gate (Kiln-internal; kiln_engine_native.h ecs_gate_invoke).
// The portable checks are invoke.cpp; the call itself, the SEH-guarded compare and the stack range are invoke_win.cpp
// (host tests link tests/host/invoke_host.cpp). Everything here runs inside a gate's hold: no allocation, no lock, no
// wait, no log, no KilnApi call, only bounded loops (tests/lint_gate.py).
#pragma once
#include <cstddef>
#include <cstdint>

#include "gate.h"
#include "include/kiln_engine_layout.h"
#include "include/kiln_engine_view.h"

namespace kiln::engine {

// ecs_gate_invoke's body for every op. `layout` NULL: no exact profile. `gate` NULL only for op Status (the member
// checks the records' sizes and the op). Fills `out` and keeps its size; never releases the gate.
void gate_invoke(const Layout* layout, const GateState* gate, const KilnEngineCall& call, KilnEngineCallResult& out) noexcept;
// Recipe `id` of `layout` when it is well formed (CallRecipe's rules, every range inside the image, the 64 KiB cap),
// else nullptr.
const CallRecipe* call_recipe(const Layout* layout, std::uint32_t id) noexcept;
// An exception reached the call's frame: latch every engine call for the session and note it for the log (called by
// invoke_win.cpp's filter, for every code it sees, and after a faulted call).
void call_fault_seen(std::uint32_t exception) noexcept;
// The latch's note for kiln.engine's log line after the gate's release: true once per latch, with its cause.
struct CallNote {
    Reason reason;
    std::uint32_t recipe, exception;
};
bool gate_invoke_take_note(CallNote& note) noexcept;

#ifdef KILN_ENGINE_TEST_RECIPES
// Test builds only (kiln-engine-test.dll, host tests): recipes to use instead of the profile's (NULL: the profile's).
// The shipped kiln-engine.dll is built without KILN_ENGINE_TEST_RECIPES.
void gate_invoke_set_test_recipes(const CallRecipes* recipes) noexcept;
#endif

namespace call {
struct Return {
    std::uint64_t value;
    std::uint32_t faulted, exception;
};
// `entry`(args[0..7]) as x64 __fastcall under SEH. `exception`: the code of an exception that reached its frame, if one
// did; `faulted` 1 when it was handled there (every code except stack overflow and the debugger's breakpoint and
// single-step codes, which go on); `exception` without `faulted`: one went on and an outer handler resumed the call.
Return invoke(std::uintptr_t entry, const std::uint64_t* args) noexcept;
// memcmp of `size` live bytes at `address` against `expected` under SEH; false on a difference or a fault.
bool same(std::uintptr_t address, const std::uint8_t* expected, std::size_t size) noexcept;
// The calling thread's stack [low, high); false when it cannot be told.
bool stack_range(std::uintptr_t& low, std::uintptr_t& high) noexcept;
#ifdef _WIN32
// The call's exception filter decision for `code` (EXCEPTION_EXECUTE_HANDLER or EXCEPTION_CONTINUE_SEARCH).
int filter_decision(unsigned long code) noexcept;
#endif
}  // namespace call

}  // namespace kiln::engine
