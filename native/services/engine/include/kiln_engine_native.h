// SPDX-License-Identifier: MIT
// kiln.internal.engine: Kiln-internal engine primitives for Kiln's own native services. NOT a public ABI: not in
// native/core/abi, not in kiln-abi, not in the SDK, and it may change between Kiln builds. The core resolves the name
// only through the KilnApi it hands to DLLs of kiln/services and accepts its publication only through that KilnApi, for
// such a DLL's owner (native/core/src/api.cpp; docs/kiln/services/README.md "Kiln-internal tables"). Plugins, managed mods and
// foreign DLLs never see it: a plugin holding the engine's Scene word would freeze the server.
//
// Consumers check header.version == KILN_ENGINE_NATIVE_VERSION, header.size covering every member they use,
// layout_size == sizeof(kiln::engine::Layout) and profile_size == sizeof(kiln::engine::Profile) (both sides are built
// from one tree); a mismatch is refused loudly by kiln_engine_native() below, and the consumer reports its own row
// unavailable with that reason.
//
// Threads: every read member here is callable from ANY thread, world threads included: no allocation, no host lock, no wait. The gate members refuse on a world's own
// thread (see ecs_gate_acquire), and so does the content worker's lifetime guard when it takes the registry word itself
// (see resource_guard_acquire). Engine memory is read under SEH (a freed or
// protected page fails the call, never the process); the dedicated server's view takes the engine's thread-registry
// word Shared for its traversal with a nonblocking try (refused with ecs-thread-registry-busy while a thread registers). Views are optimistic copies (resolved twice, published only when equal): a view is evidence of
// one instant, never a lock; writers re-validate under the Scene gate.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "kiln_api.h"
#include "kiln_engine_layout.h"
#include "kiln_engine_view.h"

#define KILN_ENGINE_NATIVE_SERVICE "kiln.internal.engine"
#define KILN_ENGINE_NATIVE_VERSION 1u

extern "C" {
struct KilnEngineNativeApi {
    KilnServiceHeader header;
    std::uint32_t layout_size, profile_size; /* sizeof(kiln::engine::Layout), sizeof(kiln::engine::Profile) */
    /* The selected exact profile's validated layout, immutable for the process; NULL without an exact profile. */
    const kiln::engine::Layout*(KILN_CALL* layout)(void);

    /* ---- reads ---- */
    /* Copy `bytes` from engine memory at `address` under SEH. KILN_OK; KILN_ERR_UNAVAILABLE: null, overflowing or
     * unreadable range (out undefined); KILN_ERR_INVALID_ARGUMENT: out NULL with bytes > 0; bytes 0 is a no-op. */
    KilnResult(KILN_CALL* copy)(std::uintptr_t address, void* out, std::size_t bytes);
    /* The owner/Scene/ECS view: kind 0 = ResolveEcs (the local authority when this process hosts one,
     * else the client replica), kind 1 = ResolveClientEcs (the client's main world; refused on a dedicated server).
     * KILN_OK (*reason 0); KILN_ERR_UNAVAILABLE with *reason = kiln::engine::Reason (its refusal string via
     * reason_name()); KILN_ERR_INVALID_ARGUMENT: out/reason NULL or an unknown kind. */
    KilnResult(KILN_CALL* ecs_view)(std::uint32_t kind, kiln::engine::EcsView* out, std::uint32_t* reason);
    /* EcsComponentWriteSupported: 1 when the profile has a write gate, `expected` is a local-authority
     * view still equal to the engine's (resolved twice), and `component`'s registration index carries neither
     * change-tracking category bit; else 0. A hint for planning a write; the Scene gate re-validates. */
    std::int32_t(KILN_CALL* ecs_write_supported)(const kiln::engine::EcsView* expected, std::uint16_t component);
    /* The selected exact profile (identity, sections, verbatim leaves, the published attribute model),
     * immutable for the process; NULL without an exact profile. */
    const kiln::engine::Profile*(KILN_CALL* profile)(void);

    /* ---- the engine's original write gate (adapted from ShroudForge's EcsWriteGuard, lock_scene_access.cpp:84-195) ----
     * Server world only (the gate is the local authority's). Any thread except a world's own: on the server
     * world thread (inside the GameServer update, which holds the Scene word Shared for its whole length) acquire refuses
     * kiln-in-frame, on the client/prediction world thread kiln-world-thread, and when the core cannot tell
     * kiln-caller-thread-unknown, all before touching a word. Engine job workers running a world program are none of
     * these threads: they reach the exclusive try and get ecs-scene-frame-busy, as every caller does while the update
     * runs (correct: refused, no spin; do not "fix" it with a wait). Busy is the normal result: retry at a later
     * step or fall back to an in-phase path; never wait or spin for the gate.
     * Between acquire and release: no allocation, no lock, no wait, no KilnApi call, no log; prepare buffers and take
     * caller mutexes before acquiring (lock_scene_access.h:7-12). Release on every path: use kiln::engine::EcsGateScope
     * below; a Rust consumer runs its body under catch_unwind inside the scope. */
    /* EcsWriteGuard::TryAcquire against `expected` (a current local-authority view), for gate->owner at server step
     * gate->step (both set by the caller, nonzero). KILN_OK: held; KILN_ERR_UNAVAILABLE with *reason (the engine's,
     * the three kiln-* thread refusals, and Kiln's admission refusals); KILN_ERR_INVALID_ARGUMENT: a NULL argument,
     * gate->size != sizeof(EcsGate), owner or step 0, or an unknown gate->admission.
     * Kiln's admission, before any engine word, never a wait:
     * - kiln-gate-busy: another Kiln thread holds the gate (one Kiln holder at a time);
     * - kiln-gate-busy-this-step: this owner had a busy refusal (registry, Scene or kiln-gate-busy) in this server step
     *   (the live server step kiln.engine follows; the caller's step only without a tick subscription). Not for
     *   gate->admission = GateAdmission::RetryKilnHolder: no latch, and kiln-gate-busy is retried up to 128 times first;
     *   a later step clears it; successful holds never latch, so several writes per step stay possible. Nor for the one
     *   owner the core designates through kiln.internal.launch (Kiln-internal): it is admitted as RetryKilnHolder
     *   whatever gate->admission says, and gate->admission is left as set (docs/kiln/services/launch.md);
     * - kiln-gate-closed: the server world is stopping (reopens at its next STARTED), or the process is exiting.
     * While a gate is held, its thread runs at THREAD_PRIORITY_ABOVE_NORMAL at least (raised before the first word, restored after
     * the last), above the engine's normal-priority threads (the GameServer thread is created at NORMAL). */
    KilnResult(KILN_CALL* ecs_gate_acquire)(const kiln::engine::EcsView* expected, kiln::engine::EcsGate* gate,
                                            std::uint32_t* reason);
    /* EcsWriteGuard::Validate: 1 while the GameServer thread, its running Scene and the expected authority view still
     * hold; call it before each mutation and after the last. */
    std::int32_t(KILN_CALL* ecs_gate_validate)(const kiln::engine::EcsGate* gate);
    /* EcsWriteGuard::ComponentSupported: 1 when `component`'s registration carries neither change-tracking category. */
    std::int32_t(KILN_CALL* ecs_gate_component_supported)(const kiln::engine::EcsGate* gate, std::uint16_t component);
    /* EcsWriteGuard::TrackChange: the engine's own change recorder for `entity`, called only when its guard bytes match
     * and the component is untracked; 0 when refused or when the recorder faulted (SEH). */
    std::int32_t(KILN_CALL* ecs_gate_track_change)(const kiln::engine::EcsGate* gate, std::uintptr_t entity,
                                                   std::uint16_t component);
    /* guarded_copy (adapted from ShroudForge) under a held gate. KILN_OK (or bytes 0); KILN_ERR_BUFFER_TOO_SMALL: bytes over
     * gate->maximum_bytes, refused before any mutation (ecs-write-size-exceeds-gate-budget);
     * KILN_ERR_UNAVAILABLE: the copy faulted and a partial write may have happened (restore the original, read it back
     * with copy, latch "uncertain" when the readback does not prove it: write_entity_components.cpp:87-98);
     * KILN_ERR_INVALID_ARGUMENT: no held gate, or data NULL with bytes > 0. */
    KilnResult(KILN_CALL* ecs_gate_write)(const kiln::engine::EcsGate* gate, std::uintptr_t address, const void* data,
                                          std::size_t bytes);
    /* EcsWriteGuard::Release: the Scene word, then the registry word, then Kiln's holder flag and the priority restore.
     * Idempotent; a no-op on a refused or zeroed gate. It leaves its measurements in gate->hold_ns (the Scene word's
     * exclusive interval, ending at that word's release) and gate->restore_ns (the priority restore, timed apart), kept
     * until the next acquire (kiln_engine_view.h EcsGate; GitHub #58). */
    void(KILN_CALL* ecs_gate_release)(kiln::engine::EcsGate* gate);

    /* ---- the content worker's lifetime (Kiln-internal; adapted from ShroudForge's
     * ResourceLifeGuard, engine-context/hold_resource_lifetime.{h,cpp}) ----
     * Holds the engine's thread-registry word Shared, so the registered resource content worker cannot be unregistered,
     * and its reader and bulk payload destroyed, while the guard is held. It guards that lifetime only: it freezes no
     * resource bytes, keys or map entries, so a member re-checks what it read before it trusts it, and scalar atomicity
     * is the member's own (a whole-op gate hold). Use kiln::engine::ResourceGuardScope below. */
    /* TryAcquire for the content worker whose service is `expected_service` and whose reader is `expected_reader` (both
     * nonzero, resolved by the caller from the resource layout). Never a wait; nothing is allocated; on every refusal the
     * record is left released.
     * - held_or_null = a held gate (borrowed, TryAcquire(scene, ...)): call it inside that gate's hold, after acquiring it;
     *   it borrows the gate's registry share and takes no word, checks the gate still validates, and makes no KilnApi
     *   call. Release the guard before the gate; once the gate is released the guard validates and contains nothing
     *   (until that same record holds the same word again; validate then re-checks the worker as at acquire).
     * - held_or_null = NULL (owned, TryAcquire(...)): outside any gate. It refuses on a world's own thread with the gate's
     *   thread refusals (kiln-in-frame, kiln-world-thread, kiln-caller-thread-unknown) before any engine read, resolves
     *   the owners, then try-acquires the registry word Shared itself (ecs-thread-registry-busy while a thread registers
     *   or unregisters). While held it blocks thread (un)registration in the whole process: keep the hold short.
     *   It has none of the gate's Kiln admission: no kiln-gate-closed (stopping server world, exit), no busy latch and
     *   no priority raise (the holder keeps its thread's priority; a Kiln worker's is below normal). It is for bounded
     *   reads, as ShroudForge uses it (scalar batch, item lookup, status): never hold it across a bulk copy (ShroudForge's
     *   full resource read takes no lifetime guard, read_loaded_resource.cpp), and write engine memory only inside a
     *   gate's hold with the guard borrowed from that gate, so the gate's closing, latch and priority
     *   raise cover every write.
     * KILN_OK: held; KILN_ERR_UNAVAILABLE with *reason (kiln::engine::Reason: the resource-lifetime-* refusals and the
     * engine context's own); KILN_ERR_INVALID_ARGUMENT: out, reason NULL, out->size != sizeof(ResourceGuard), a service
     * or reader of 0, or a gate record of the wrong size. */
    KilnResult(KILN_CALL* resource_guard_acquire)(const kiln::engine::EcsGate* held_or_null, std::uintptr_t expected_service,
                                                  std::uintptr_t expected_reader, kiln::engine::ResourceGuard* out,
                                                  std::uint32_t* reason);
    /* Validate: 1 while the guard is held (and a borrowed guard's gate still is), the worker thread, the service's
     * initialized flag, reader, payload and payload size are as acquired, and the owners resolve to the same; else 0.
     * Call it after the reads a member trusts. Any thread; no allocation, no lock, no wait. */
    std::int32_t(KILN_CALL* resource_guard_validate)(const kiln::engine::ResourceGuard* guard);
    /* Contains: 1 when [address, address + size) lies inside the held guard's payload (size 0 at its end included). */
    std::int32_t(KILN_CALL* resource_guard_contains)(const kiln::engine::ResourceGuard* guard, std::uintptr_t address,
                                                     std::size_t size);
    /* Release: the registry word only when the guard owns it (a borrowed guard touches no word). Idempotent; a no-op on
     * a refused or zeroed record. */
    void(KILN_CALL* resource_guard_release)(kiln::engine::ResourceGuard* guard);

    /* ---- an engine call inside a held write gate (Kiln-internal; adapted from ShroudForge's
     * in-hold invoke, impact/submit_building_prop_spawn.cpp:776-885) ----
     * call->op Invoke: calls the profile's reviewed recipe call->recipe (kiln_engine_layout.h CallRecipe) at
     * image + ranges[0].rva with call->args (x64 __fastcall, up to 8 integer or pointer arguments) under SEH, inside
     * `gate`'s hold. Never releases the gate (the member's EcsGateScope does, on every path). In order, refused with
     * nothing called (KILN_ERR_UNAVAILABLE, out->outcome Refused, out->reason):
     *   engine-call-gate-not-held; kiln-gate-closed (the server world began stopping or the process exits while held);
     *   engine-call-latched; engine-call-recipe-unavailable; engine-call-target (the gate's image is not the loaded game
     *   image, mem::image_base()); engine-call-argument-invalid / -on-stack (KilnEngineCall);
     *   engine-call-gate-invalid (ecs_gate_validate); engine-code-changed (a live byte of a range differs from
     *   call->code, compared under SEH: ShroudForge's validate_code before invoke).
     * Then the call. KILN_OK, outcome Returned, out->value = the recipe's result bits: it returned, the live code still
     * equals call->code and the gate still validates. KILN_ERR_UNAVAILABLE, outcome Uncertain: an exception reached the
     * call (engine-call-faulted, out->exception its code; out->faulted 1 when it was handled at the call's frame, 0 when it
     * went on and an outer handler resumed the call, which then returned), or the code or the gate changed during it.
     * Every exception is caught at the call's frame except EXCEPTION_STACK_OVERFLOW and the debugger's breakpoint and
     * single-step codes (native and WOW64), which go on to an outer handler; any exception that reaches the frame
     * latches. Uncertain latches EVERY engine call for the rest of the server session (cleared by the server world's
     * next STARTED; fail closed: a latch that cannot be recorded latches for the process), and kiln.engine logs it once
     * from ecs_gate_release. Plain gated writes are not latched by it.
     * What the caller does first, before the gate: checks the recipe's code shape on ORIGINAL bytes
     * (kiln.hooks read_original), checks kiln.hooks code_state ORIGINAL for every range (a Kiln hook or a foreign patch
     * is refused there: engine-code-patched), then captures the live bytes into call->code; allocates every buffer; and
     * may ask op Status. Inside the hold, as for every gate member: no allocation, lock, wait, log or KilnApi call.
     * call->op Latch (a held gate): the member's own check after a call failed; latches like a fault. Returned.
     * call->op Status (gate may be NULL; any thread): KILN_OK, or KILN_ERR_UNAVAILABLE with engine-call-latched or
     * engine-call-recipe-unavailable (call->recipe nonzero and not a recipe of this build).
     * KILN_ERR_INVALID_ARGUMENT: call or out NULL or of the wrong size, an unknown op, a gate record of the wrong size,
     * or no gate for Invoke or Latch. Ported profiles carry no recipe yet (the services that call one add theirs). */
    KilnResult(KILN_CALL* ecs_gate_invoke)(const kiln::engine::EcsGate* gate, const kiln::engine::KilnEngineCall* call,
                                           kiln::engine::KilnEngineCallResult* out);
};
}
static_assert(offsetof(KilnEngineNativeApi, layout) == 16, "kiln.internal.engine v1 prefix");
static_assert(offsetof(KilnEngineNativeApi, copy) == 24 && offsetof(KilnEngineNativeApi, ecs_view) == 32 &&
                  offsetof(KilnEngineNativeApi, ecs_write_supported) == 40 && offsetof(KilnEngineNativeApi, profile) == 48,
              "kiln.internal.engine read members");
static_assert(offsetof(KilnEngineNativeApi, ecs_gate_acquire) == 56 && offsetof(KilnEngineNativeApi, ecs_gate_validate) == 64 &&
                  offsetof(KilnEngineNativeApi, ecs_gate_component_supported) == 72 &&
                  offsetof(KilnEngineNativeApi, ecs_gate_track_change) == 80 &&
                  offsetof(KilnEngineNativeApi, ecs_gate_write) == 88 && offsetof(KilnEngineNativeApi, ecs_gate_release) == 96,
              "kiln.internal.engine gate members");
static_assert(offsetof(KilnEngineNativeApi, resource_guard_acquire) == 104 &&
                  offsetof(KilnEngineNativeApi, resource_guard_validate) == 112 &&
                  offsetof(KilnEngineNativeApi, resource_guard_contains) == 120 &&
                  offsetof(KilnEngineNativeApi, resource_guard_release) == 128,
              "kiln.internal.engine resource lifetime members");
static_assert(offsetof(KilnEngineNativeApi, ecs_gate_invoke) == 136 && sizeof(KilnEngineNativeApi) == 144,
              "kiln.internal.engine engine call member");

// The table, or nullptr with a reason in `why` (cap bytes, NUL-terminated) and one log line through api->log for
// `owner`. `api` must be the KilnApi a kiln/services DLL received in kiln_plugin_main; any other KilnApi never resolves
// it. Looked up with min_version 1, so an older or newer provider is named "another Kiln build", not "missing".
inline const KilnEngineNativeApi* kiln_engine_native(const KilnApi* api, KilnOwner owner, std::size_t need, char* why,
                                                     std::size_t cap) {
    const auto say = [&](const char* text) {
        if (why && cap) std::snprintf(why, cap, "%s", text);
        if (api && api->log) {
            char line[256];
            const int n = std::snprintf(line, sizeof line, "kiln.internal.engine refused: %s", text);
            api->log(owner, KILN_LOG_WARN, {line, n > 0 ? static_cast<std::size_t>(n) : 0});
        }
        return static_cast<const KilnEngineNativeApi*>(nullptr);
    };
    if (!api || !api->service) return say("no KilnApi");
    const auto* t = static_cast<const KilnEngineNativeApi*>(
        api->service({KILN_ENGINE_NATIVE_SERVICE, sizeof KILN_ENGINE_NATIVE_SERVICE - 1}, 1));
    if (!t)
        return say("kiln.internal.engine is not here (kiln-engine.dll missing, of another build, or not a Kiln "
                   "service caller)");
    if (t->header.version != KILN_ENGINE_NATIVE_VERSION || t->header.size < need ||
        t->header.size < offsetof(KilnEngineNativeApi, layout) + sizeof t->layout ||
        t->layout_size != sizeof(kiln::engine::Layout) || t->profile_size != sizeof(kiln::engine::Profile))
        return say("kiln.internal.engine is from another Kiln build (version or layout size mismatch)");
    return t;
}

namespace kiln::engine {
// Holds the engine's write gate for one C++ scope; the destructor releases it on every path, exceptions included.
// `api` must carry the gate members (kiln_engine_native() with need = sizeof(KilnEngineNativeApi)).
class EcsGateScope {
public:
    // `owner`: the calling KilnOwner; `step`: the server step it observed (KilnTickInfo.step); both nonzero.
    // `admission`: GateAdmission::RetryKilnHolder for a caller that must never be latched, else KilnMod.
    EcsGateScope(const KilnEngineNativeApi& api, const EcsView& expected, std::uint64_t owner, std::uint64_t step,
                 GateAdmission admission = GateAdmission::KilnMod) noexcept
        : api_(api) {
        gate_.size = sizeof gate_;
        gate_.owner = owner;
        gate_.step = step;
        gate_.admission = static_cast<std::uint32_t>(admission);
        result_ = api_.ecs_gate_acquire(&expected, &gate_, &reason_);
    }
    ~EcsGateScope() { api_.ecs_gate_release(&gate_); }
    EcsGateScope(const EcsGateScope&) = delete;
    EcsGateScope& operator=(const EcsGateScope&) = delete;

    bool held() const noexcept { return result_ == KILN_OK && gate_.held; }
    KilnResult result() const noexcept { return result_; }
    Reason reason() const noexcept { return static_cast<Reason>(reason_); }
    const EcsGate& gate() const noexcept { return gate_; }
    bool validate() const noexcept { return held() && api_.ecs_gate_validate(&gate_) == 1; }
    bool component_supported(std::uint16_t component) const noexcept {
        return held() && api_.ecs_gate_component_supported(&gate_, component) == 1;
    }
    bool track_change(std::uintptr_t entity, std::uint16_t component) const noexcept {
        return held() && api_.ecs_gate_track_change(&gate_, entity, component) == 1;
    }
    KilnResult write(std::uintptr_t address, const void* data, std::size_t bytes) const noexcept {
        return held() ? api_.ecs_gate_write(&gate_, address, data, bytes) : KILN_ERR_INVALID_ARGUMENT;
    }
    // An engine call (op Invoke) or the member's latch (op Latch) inside this hold; `out.size` set by the caller.
    // The gate stays held whatever the outcome; the destructor releases it.
    KilnResult invoke(const KilnEngineCall& call, KilnEngineCallResult& out) const noexcept {
        return api_.ecs_gate_invoke(&gate_, &call, &out);
    }
    void release() noexcept { api_.ecs_gate_release(&gate_); }

private:
    const KilnEngineNativeApi& api_;
    EcsGate gate_{};
    KilnResult result_ = KILN_ERR_UNAVAILABLE;
    std::uint32_t reason_ = 0;
};

// Holds the content worker's lifetime guard for one C++ scope; the destructor releases it on every path,
// exceptions included. `api` must carry the guard members (kiln_engine_native() with need = sizeof(KilnEngineNativeApi)).
// The order with the gate is fixed by declaration: an EcsGateScope first, then the borrowed ResourceGuardScope, so the
// guard is released before the gate. The owned form is for callers that hold no gate.
class ResourceGuardScope {
public:
    // Owned: takes the registry word Shared itself (never waits); refused on a world's own thread. Bounded reads only, no
    // gate admission (see resource_guard_acquire).
    ResourceGuardScope(const KilnEngineNativeApi& api, std::uintptr_t expected_service, std::uintptr_t expected_reader) noexcept
        : api_(api) {
        acquire(nullptr, expected_service, expected_reader);
    }
    // Borrowed: inside `gate`'s hold (declare it after `gate`); refused with resource-lifetime-gate-not-held when the
    // gate is not held.
    ResourceGuardScope(const KilnEngineNativeApi& api, const EcsGateScope& gate, std::uintptr_t expected_service,
                       std::uintptr_t expected_reader) noexcept
        : api_(api) {
        acquire(&gate.gate(), expected_service, expected_reader);
    }
    ~ResourceGuardScope() { api_.resource_guard_release(&guard_); }
    ResourceGuardScope(const ResourceGuardScope&) = delete;
    ResourceGuardScope& operator=(const ResourceGuardScope&) = delete;

    bool held() const noexcept { return result_ == KILN_OK && guard_.held; }
    KilnResult result() const noexcept { return result_; }
    Reason reason() const noexcept { return static_cast<Reason>(reason_); }
    const ResourceGuard& guard() const noexcept { return guard_; }
    std::uintptr_t payload() const noexcept { return held() ? guard_.payload : 0; }  // ShroudForge's PayloadBase()
    bool validate() const noexcept { return held() && api_.resource_guard_validate(&guard_) == 1; }
    bool contains(std::uintptr_t address, std::size_t size) const noexcept {
        return held() && api_.resource_guard_contains(&guard_, address, size) == 1;
    }
    void release() noexcept { api_.resource_guard_release(&guard_); }

private:
    void acquire(const EcsGate* gate, std::uintptr_t service, std::uintptr_t reader) noexcept {
        guard_.size = sizeof guard_;
        result_ = api_.resource_guard_acquire(gate, service, reader, &guard_, &reason_);
    }
    const KilnEngineNativeApi& api_;
    ResourceGuard guard_{};
    KilnResult result_ = KILN_ERR_UNAVAILABLE;
    std::uint32_t reason_ = 0;
};
}  // namespace kiln::engine
