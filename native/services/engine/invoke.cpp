// SPDX-License-Identifier: MIT
// One engine call inside a held write gate (Kiln-internal; kiln_engine_native.h ecs_gate_invoke).
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer), modules/game-runtime/native/impact/
// submit_building_prop_spawn.cpp:776-885 (SubmitBuildingPropSpawn: validate_code, i.e. OriginalCode::EvidenceUnchanged,
// before the call; the queue buffer checked against the thread's stack; invoke under SEH; scene.Validate() and
// validate_code again after it; MarkSessionWriteUncertain when the outcome is not proven). Differences, all safety-only:
// - the function is a reviewed recipe of the exact profile (a recipe id, never a raw address), and the code compared is
//   every range of its reviewed call graph, captured by the caller before the gate;
// - every caller buffer and the capture are off the calling thread's stack and outside the game image (ShroudForge
//   checks only the command queue against the stack and passes stack locals; a Kiln member prepares its buffers before
//   the gate), and no buffer overlaps the capture;
// - an exception that reaches the call, or a change of the code or the gate across it, latches every engine call for
//   the rest of the server session (ShroudForge marks the session's writes uncertain), and is noted for one log line
//   after the gate's release;
// - every refusal has its own reason.
// Inside the hold: no allocation, no lock, no wait, no log, no KilnApi call, loops bounded by the recipe's counts
// (tests/lint_gate.py). The gate is never released here.
#include "invoke.h"

#include <atomic>

#include "memory.h"

namespace kiln::engine {
namespace {
// The latch's note for the log line after the gate's release (engine.cpp), and the recipe of the call in flight for an
// exception filter's note. One gate holder at a time makes these the holder's own; a note is only ever read after
// the release.
std::atomic<std::uint32_t> g_note{0}, g_note_reason{0}, g_note_recipe{0}, g_note_exception{0}, g_in_flight{0};

#ifdef KILN_ENGINE_TEST_RECIPES
std::atomic<const CallRecipes*> g_test_recipes{nullptr};
#endif

const CallRecipes* recipes_of(const Layout* layout) noexcept {
#ifdef KILN_ENGINE_TEST_RECIPES
    if (const CallRecipes* recipes = g_test_recipes.load(std::memory_order_acquire)) return layout ? recipes : nullptr;
#endif
    return layout ? &layout->calls : nullptr;
}

void note(Reason why, std::uint32_t recipe, std::uint32_t exception) noexcept {
    g_note_reason.store(static_cast<std::uint32_t>(why), std::memory_order_relaxed);
    g_note_recipe.store(recipe, std::memory_order_relaxed);
    g_note_exception.store(exception, std::memory_order_relaxed);
    g_note.store(1, std::memory_order_release);
}

void latch(Reason why, std::uint32_t recipe, std::uint32_t exception) noexcept {
    gate_calls_latch();
    note(why, recipe, exception);
}

bool held(const GateState* gate) noexcept { return gate && gate->held && gate->registry_lock && gate->scene_lock; }

// Both ranges are non-empty and do not overflow (checked before).
bool overlaps(std::uintptr_t a, std::size_t a_size, std::uintptr_t b, std::size_t b_size) noexcept {
    return a < b + b_size && b < a + a_size;
}

// The live bytes of every range equal the capture, range after range (ShroudForge's EvidenceUnchanged).
bool unchanged(const CallRecipe& recipe, std::uintptr_t image, const std::uint8_t* captured) noexcept {
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < recipe.range_count && i < kCallRanges; ++i) {
        const CallRange& range = recipe.ranges[i];
        if (!call::same(image + range.rva, captured + at, range.size)) return false;
        at += range.size;
    }
    return true;
}

std::size_t code_bytes(const CallRecipe& recipe) noexcept {
    std::size_t total = 0;
    for (std::uint32_t i = 0; i < recipe.range_count && i < kCallRanges; ++i) total += recipe.ranges[i].size;
    return total;
}

std::uint64_t masked(std::uint64_t value, std::uint32_t bits) noexcept {
    return bits >= 64 ? value : bits == 0 ? 0 : value & ((std::uint64_t{1} << bits) - 1);
}
}  // namespace

const CallRecipe* call_recipe(const Layout* layout, std::uint32_t id) noexcept {
    const CallRecipes* recipes = recipes_of(layout);
    if (!recipes || !id || id > kCallRecipes) return nullptr;
    const CallRecipe& r = recipes->recipe[id - 1];
    if (r.id != id || r.arg_count > kCallArguments || r.range_count == 0 || r.range_count > kCallRanges) return nullptr;
    if (r.result_bits != 0 && r.result_bits != 8 && r.result_bits != 16 && r.result_bits != 32 && r.result_bits != 64)
        return nullptr;
    for (std::size_t i = r.arg_count; i < kCallArguments; ++i)
        if (r.arg_bytes[i]) return nullptr;
    std::size_t total = 0;
    for (std::size_t i = 0; i < kCallRanges; ++i) {
        const CallRange& range = r.ranges[i];
        if (i >= r.range_count) {
            if (range.rva || range.size) return nullptr;
            continue;
        }
        if (!range.size || range.rva >= layout->image_size || range.size > layout->image_size - range.rva ||
            range.size > kCallCodeBytes - total)
            return nullptr;
        total += range.size;
    }
    return &r;
}

void call_fault_seen(std::uint32_t exception) noexcept {
    latch(Reason::EngineCallFaulted, g_in_flight.load(std::memory_order_relaxed), exception);
}

bool gate_invoke_take_note(CallNote& out) noexcept {
    if (!g_note.exchange(0, std::memory_order_acq_rel)) return false;
    out.reason = static_cast<Reason>(g_note_reason.load(std::memory_order_relaxed));
    out.recipe = g_note_recipe.load(std::memory_order_relaxed);
    out.exception = g_note_exception.load(std::memory_order_relaxed);
    return true;
}

#ifdef KILN_ENGINE_TEST_RECIPES
void gate_invoke_set_test_recipes(const CallRecipes* recipes) noexcept {
    g_test_recipes.store(recipes, std::memory_order_release);
}
#endif

void gate_invoke(const Layout* layout, const GateState* gate, const KilnEngineCall& record, KilnEngineCallResult& out) noexcept {
    // One copy of the caller's record (no allocation): every check and the call use the same values, so a record that
    // changes meanwhile cannot pass a checked argument to the check and another one to the engine.
    const KilnEngineCall call = record;
    const std::uint32_t size = out.size;
    out = KilnEngineCallResult{};
    out.size = size;
    out.outcome = static_cast<std::uint32_t>(CallOutcome::Refused);
    const auto refuse = [&out](Reason why) { out.reason = static_cast<std::uint32_t>(why); };
    const auto returned = [&out](std::uint64_t value) {
        out.outcome = static_cast<std::uint32_t>(CallOutcome::Returned);
        out.value = value;
    };
    const auto uncertain = [&out](Reason why) {
        out.outcome = static_cast<std::uint32_t>(CallOutcome::Uncertain);
        out.reason = static_cast<std::uint32_t>(why);
    };
    if (call.op == static_cast<std::uint32_t>(CallOp::Status)) {
        if (gate_calls_latched()) return refuse(Reason::EngineCallLatched);
        if (call.recipe && !call_recipe(layout, call.recipe)) return refuse(Reason::EngineCallRecipeUnavailable);
        return returned(0);
    }
    if (call.op == static_cast<std::uint32_t>(CallOp::Latch)) {
        if (!held(gate)) return refuse(Reason::EngineCallGateNotHeld);
        latch(Reason::EngineCallMemberUncertain, call.recipe, 0);
        return returned(0);
    }
    if (call.op != static_cast<std::uint32_t>(CallOp::Invoke)) return refuse(Reason::EngineCallArgumentInvalid);

    // ---- refused, nothing called ----
    if (!held(gate)) return refuse(Reason::EngineCallGateNotHeld);
    if (gate_closed()) return refuse(Reason::KilnGateClosed);
    if (gate_calls_latched()) return refuse(Reason::EngineCallLatched);
    const CallRecipe* recipe = call_recipe(layout, call.recipe);
    if (!recipe) return refuse(Reason::EngineCallRecipeUnavailable);
    const std::uintptr_t image = gate->expected.owners.image;
    const std::size_t image_size = layout->image_size;
    const std::size_t total = code_bytes(*recipe);
    const std::uint8_t* captured = call.code;
    const auto capture = reinterpret_cast<std::uintptr_t>(captured);
    if (!image || image > UINTPTR_MAX - image_size) return refuse(Reason::EngineCallRecipeUnavailable);
    // The call target is image + ranges[0].rva: the gate's image must be the loaded game image itself, not only a record
    // that validates (defense in depth; the gate was acquired against mem::image_base()).
    if (image != mem::image_base()) return refuse(Reason::EngineCallTarget);
    if (call.arg_count != recipe->arg_count || call.buffer_count > kCallArguments || !capture || call.code_bytes != total ||
        capture > UINTPTR_MAX - total)
        return refuse(Reason::EngineCallArgumentInvalid);
    for (std::size_t i = call.arg_count; i < kCallArguments; ++i)
        if (call.args[i]) return refuse(Reason::EngineCallArgumentInvalid);
    std::uintptr_t low = 0, high = 0;
    if (!call::stack_range(low, high) || low >= high) return refuse(Reason::EngineCallArgumentOnStack);
    if (overlaps(capture, total, image, image_size)) return refuse(Reason::EngineCallArgumentInvalid);
    if (overlaps(capture, total, low, high - low)) return refuse(Reason::EngineCallArgumentOnStack);
    // every caller buffer: the pointer arguments, then the further buffers
    for (std::size_t i = 0; i < kCallArguments + kCallArguments; ++i) {
        std::uintptr_t address = 0;
        std::size_t bytes = 0;
        if (i < kCallArguments) {
            if (i >= call.arg_count || !recipe->arg_bytes[i]) continue;
            address = static_cast<std::uintptr_t>(call.args[i]);
            bytes = recipe->arg_bytes[i];
        } else {
            if (i - kCallArguments >= call.buffer_count) break;
            address = call.buffers[i - kCallArguments].address;
            bytes = call.buffers[i - kCallArguments].size;
        }
        if (!address || !bytes || address > UINTPTR_MAX - bytes) return refuse(Reason::EngineCallArgumentInvalid);
        if (overlaps(address, bytes, low, high - low)) return refuse(Reason::EngineCallArgumentOnStack);
        if (overlaps(address, bytes, image, image_size) || overlaps(address, bytes, capture, total))
            return refuse(Reason::EngineCallArgumentInvalid);
    }
    if (!gate_validate(layout, *gate, image)) return refuse(Reason::EngineCallGateInvalid);
    if (!unchanged(*recipe, image, captured)) return refuse(Reason::EngineCodeChanged);

    // ---- the call ----
    std::uint64_t args[kCallArguments];
    for (std::size_t i = 0; i < kCallArguments; ++i) args[i] = call.args[i];
    const std::uint32_t id = call.recipe;
    g_in_flight.store(id, std::memory_order_relaxed);
    const call::Return r = call::invoke(image + recipe->ranges[0].rva, args);
    g_in_flight.store(0, std::memory_order_relaxed);

    // ---- after it: the outcome is proven or every engine call is latched ----
    // An exception reached the call: handled at its frame (faulted), or passed on and resumed further out (the call then
    // returned, but nothing proves what the engine did around it).
    if (r.faulted || r.exception) {
        latch(Reason::EngineCallFaulted, id, r.exception);  // invoke_win.cpp's filter latched already; the host has none
        out.faulted = r.faulted ? 1 : 0;
        out.exception = r.exception;
        return uncertain(Reason::EngineCallFaulted);
    }
    if (!unchanged(*recipe, image, captured)) {
        latch(Reason::EngineCallCodeChangedDuringCall, id, 0);
        return uncertain(Reason::EngineCallCodeChangedDuringCall);
    }
    if (!gate_validate(layout, *gate, image)) {
        latch(Reason::EngineCallGateChangedDuringCall, id, 0);
        return uncertain(Reason::EngineCallGateChangedDuringCall);
    }
    return returned(masked(r.value, recipe->result_bits));
}

}  // namespace kiln::engine
