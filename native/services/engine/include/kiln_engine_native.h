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
// thread (see ecs_gate_acquire). Engine memory is read under SEH (a freed or
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
     *   a later step clears it; successful holds never latch, so several writes per step stay possible;
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
    /* EcsWriteGuard::Release: the Scene word, then the registry word. Idempotent; a no-op on a refused or zeroed gate. */
    void(KILN_CALL* ecs_gate_release)(kiln::engine::EcsGate* gate);
};
}
static_assert(offsetof(KilnEngineNativeApi, layout) == 16, "kiln.internal.engine v1 prefix");
static_assert(offsetof(KilnEngineNativeApi, copy) == 24 && offsetof(KilnEngineNativeApi, ecs_view) == 32 &&
                  offsetof(KilnEngineNativeApi, ecs_write_supported) == 40 && offsetof(KilnEngineNativeApi, profile) == 48,
              "kiln.internal.engine read members");
static_assert(offsetof(KilnEngineNativeApi, ecs_gate_acquire) == 56 && offsetof(KilnEngineNativeApi, ecs_gate_validate) == 64 &&
                  offsetof(KilnEngineNativeApi, ecs_gate_component_supported) == 72 &&
                  offsetof(KilnEngineNativeApi, ecs_gate_track_change) == 80 &&
                  offsetof(KilnEngineNativeApi, ecs_gate_write) == 88 && offsetof(KilnEngineNativeApi, ecs_gate_release) == 96 &&
                  sizeof(KilnEngineNativeApi) == 104,
              "kiln.internal.engine gate members");

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
    void release() noexcept { api_.ecs_gate_release(&gate_); }

private:
    const KilnEngineNativeApi& api_;
    EcsGate gate_{};
    KilnResult result_ = KILN_ERR_UNAVAILABLE;
    std::uint32_t reason_ = 0;
};
}  // namespace kiln::engine
