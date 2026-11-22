// SPDX-License-Identifier: MIT
// kiln.internal.engine records: the owner/Scene/ECS view, the gate and content-worker lifetime guard records, the
// engine-call records, and the reasons a view, a gate, a guard or an engine call is refused. Kiln-internal (see kiln_engine_native.h). Adapted from ShroudForge (MIT,
// Copyright (c) 2026 bonsaibauer), modules/game-runtime/native/engine-context/engine_owners.h:9-56 (EcsRole, Owners,
// EcsView); the reason names are ShroudForge's own refusal strings (the fail("...") literals of engine-context/*.cpp),
// one Kiln code each, except Kiln's own kiln-* and resource-lifetime-* names (marked below).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace kiln::engine {

enum class Role : std::uint32_t { ClientReplica = 0, LocalAuthority = 1 };

struct Owners {
    std::uintptr_t image, root, client, controller, main_owner;
    std::uint32_t dedicated_server, reserved;
    std::uintptr_t world_context;
    std::array<std::uint64_t, 2> world_uuid;
    bool operator==(const Owners&) const = default;
};

// Engine addresses for Kiln's own services; never handed to a plugin. No layout here: use layout().
struct EcsView {
    Owners owners;
    Role role;
    std::uint32_t reserved;
    std::uintptr_t scene_owner, scene_entries, scene_entry, scene_context;
    std::array<std::uint64_t, 2> scene_uuid;
    std::uintptr_t execution_owner, registry_owner, manager, entity_table;
    bool operator==(const EcsView&) const = default;
};

// The engine's original write gate as one caller-owned record (ShroudForge's EcsWriteGuard state,
// lock_scene_access.h:20-40, without its allocation). Before acquiring: `size = sizeof(EcsGate)`, `owner` = the
// calling KilnOwner, `step` = the server step the caller observed (KilnTickInfo.step), both nonzero, the rest zero
// (`hold_ns` and `restore_ns` may hold a previous release's: acquire resets them); release keeps size, owner, step,
// admission and its own two measurements, so a record can be reused at a later step. Never edit it while held.
// `maximum_bytes` is the profile's write budget per component (the member refuses larger writes with
// ecs-write-size-exceeds-gate-budget). `holder`, `boosted` and `saved_priority` are the gate's own. `admission` is
// the caller's admission mode (GateAdmission, set before acquiring, kept by release).
// The release's measurements (GitHub #58), read after release, kept until the next acquire; a release of a record that
// holds nothing (a second release, after a refusal before any word) measures nothing and keeps them:
// - `hold_ns`: the Scene word's exclusive interval, from the moment its CAS won (`acquired_ns`, before revalidation) to
//   the moment it was released, read before the registry word, the holder flag and the priority restore; 0 when the
//   Scene word was not taken;
// - `restore_ns`: how long putting the thread's priority back took (SetThreadPriority; under Proton a wineserver round
//   trip), with any preemption right after it; 0 when the priority was not raised (the raise off, the thread above
//   normal already, a refusal before the raise).
struct EcsGate {
    std::uint32_t size, held;
    std::uint64_t owner, step;
    std::uint32_t holder, boosted;
    std::int32_t saved_priority;
    std::uint32_t admission;
    std::uintptr_t registry_lock, scene_lock;
    std::size_t maximum_bytes;
    std::uintptr_t manager, tracker, tracking_context, registry_owner;
    std::size_t tracking_manager_offset, tracking_registry_a, tracking_registry_b;
    std::uintptr_t thread, handle;
    std::uint64_t acquired_ns;
    EcsView expected;
    std::array<std::uint8_t, 33> tracker_bytes;
    std::uint64_t hold_ns, restore_ns;
};
// Both sides of kiln.internal.engine are built from one tree and check size == sizeof(EcsGate); x64 only.
static_assert(offsetof(EcsGate, acquired_ns) == 136 && offsetof(EcsGate, expected) == 144 &&
                  offsetof(EcsGate, tracker_bytes) == 304 && offsetof(EcsGate, hold_ns) == 344 &&
                  offsetof(EcsGate, restore_ns) == 352 && sizeof(EcsGate) == 360,
              "EcsGate");

// The content worker's lifetime guard as one caller-owned record (Kiln-internal; ShroudForge's
// ResourceLifeGuard state, engine-context/hold_resource_lifetime.cpp:6-12, without its allocation). Before acquiring:
// `size = sizeof(ResourceGuard)`; release keeps `size`, so a record can be reused. Never edit or copy it while held (a
// copy of an owned guard would release its share twice).
// The member fills the rest: `held` 1 between a successful acquire and release; `owns_registry` 1 when the guard took the
// engine's thread-registry word Shared itself (owned), 0 when it borrows the share of `gate` (borrowed; release then
// touches no word); `service`, `reader`, `thread`, `handle`, `payload` and `payload_size` are the registered content
// worker's (ShroudForge's PayloadBase() is `payload`); `owners` are the owners the guard validates against (borrowed:
// the gate's validated view's).
struct ResourceGuard {
    std::uint32_t size, held;
    std::uint32_t owns_registry, reserved;
    std::uintptr_t registry_lock;
    const EcsGate* gate;
    std::uintptr_t service, reader, thread, handle, payload;
    std::uint64_t payload_size;
    Owners owners;
};

// One engine call inside a held write gate (Kiln-internal; kiln_engine_native.h ecs_gate_invoke).
// Before calling: `size = sizeof(KilnEngineCall)` and `op`; the rest per op:
// - Invoke: `recipe` (a recipe id of the profile, kiln_engine_layout.h CallRecipe), `arg_count` (exactly the recipe's)
//   and `args` (unused ones 0); `code`/`code_bytes`: the live bytes of every range of the recipe, in range order,
//   captured by the caller BEFORE the gate, right after kiln.hooks' code_state said ORIGINAL for each range;
//   `buffers`: every further caller buffer the engine reaches through an argument buffer (`buffer_count` of them).
//   Every buffer (a pointer argument, `arg_bytes` > 0 in the recipe, or a further one) and the captured code are the
//   caller's, prepared before the gate: never on the calling thread's stack, never inside the game image, and no buffer
//   overlaps the captured code.
// - Latch: a member's own check after the call failed (the outcome is not proven): every engine call is refused for the
//   rest of the server session, as after a fault. `recipe` names the call for the log; a held gate is required.
// - Status: before acquiring the gate (any thread, no gate): engine-call-latched while calls are latched, and
//   engine-call-recipe-unavailable when `recipe` (if nonzero) is not a well-formed recipe of this build.
struct CallBuffer {
    std::uintptr_t address;
    std::size_t size;
};
enum class CallOp : std::uint32_t { Invoke = 0, Latch = 1, Status = 2 };
struct KilnEngineCall {
    std::uint32_t size, op;
    std::uint32_t recipe, arg_count;
    std::array<std::uint64_t, 8> args;
    const std::uint8_t* code;
    std::size_t code_bytes;
    std::uint32_t buffer_count, reserved;
    std::array<CallBuffer, 8> buffers;
};
// Refused: nothing was called (`reason`); Returned: the call returned and every check after it held (`value` is the
// recipe's result bits of rax), or a Latch/Status op succeeded; Uncertain: an exception reached the call (`exception`
// its code; `faulted` 1 when handled at the call's frame, 0 when it went on and was resumed further out) or a check
// after it failed (`reason`): engine state may be changed, every engine call is latched for the rest of the server
// session, and kiln.engine logs it once after the gate's release.
enum class CallOutcome : std::uint32_t { Refused = 0, Returned = 1, Uncertain = 2 };
struct KilnEngineCallResult {
    std::uint32_t size, outcome;
    std::uint32_t reason, faulted;
    std::uint32_t exception, reserved;
    std::uint64_t value;
};
static_assert(sizeof(KilnEngineCall) == 232 && sizeof(KilnEngineCallResult) == 32, "engine call records");

// Who is asking, for Kiln's own admission (EcsGate.admission):
// - KilnMod (the default): a Kiln mod or task; any busy refusal latches its owner for the rest of the server step;
// - RetryKilnHolder: never latched; a refusal caused only by another Kiln holder is retried up to 128 times with no
//   pause, before any engine word is touched (never against an engine exclusive holder). For callers that schedule
//   their own retries and must not be held back for the rest of a step.
// The gate itself admits the owner the core designates through kiln.internal.launch as RetryKilnHolder whatever its
// record names (Kiln-internal; gate.h gate_set_dialect_owner); no caller sets anything for that.
enum class GateAdmission : std::uint32_t { KilnMod = 0, RetryKilnHolder = 1 };

// The refusal strings (adapted from ShroudForge's engine context, in its source order); reason_name() returns the string.
enum class Reason : std::uint32_t {
    None = 0,
    // engine context
    EngineOwnerProfileUnavailable,   // engine-owner-profile-unavailable
    EcsReadOwnerProfileUnavailable,  // ecs-read-owner-profile-unavailable
    DedicatedServerHasNoLocalClientInput,  // dedicated-server-has-no-local-client-input
    // select_scene_context
    EcsActiveSceneListUnavailable, EcsActiveSceneListRangeInvalid, EcsSceneStateUnreadable, EcsLocalAuthorityIsAmbiguous,
    EcsReadySceneContextInvalid, EcsActiveSceneIsTransitioning, EcsReadySceneContextChanged, EcsActiveSceneListChanged,
    // resolve
    EcsThreadRegistryBusy, EcsServerWorldUnavailable, RootVtableMismatch, ClientOwnerUnavailableOrVtableMismatch,
    ClientControllerUnavailableOrVtableMismatch, ClientControllerOwnerMismatch, MainOwnerUnavailable,
    // select_execution_owner, resolve_ecs
    EcsServerWorldOwnerChanged, EcsExecutionOwnerUnavailable, EcsManagerRegistryOwnerMismatch, EcsEntityTableUnavailable,
    EcsEntityTableRangeOverflow, EcsEngineOwnerChangedDuringRead,
    // resolve_registered_scene
    EcsServerThreadLayoutInvalid, EcsThreadRegistryCountInvalid, EcsThreadRegistryRangeInvalid,
    EcsThreadRegistryRowUnreadable, EcsThreadNameUnreadable, EcsServerThreadIsAmbiguous, EcsServerThreadRangeInvalid,
    EcsServerThreadUnregistered, EcsServerThreadOwnerInvalidOrStopping, EcsServerThreadChangedDuringRead,
    // EcsWriteGuard::TryAcquire
    EcsWriteGuardAlreadyHeld, EcsWriteGateDisabled, EcsWriteRequiresCurrentLocalAuthority, EcsSceneFrameBusy,
    EcsWriteOwnerChangedOrNotAuthoritative, EcsChangeTrackerContextInvalid,
    // Kiln's own: refused before any lock word is touched
    InFrame,              // inside the server world's update: the engine holds the Scene word Shared
    WorldThread,          // on a client or prediction world thread (the game's main thread)
    CallerThreadUnknown,  // the core cannot say which thread this is (no KilnApi.on_world_thread): fail closed
    // Kiln's own gate admission, all before any engine word
    KilnHolderBusy,   // another Kiln thread holds the gate: one Kiln holder at a time
    KilnBusyLatched,  // this owner already had a busy refusal in this server step; cleared by a later step
    KilnGateClosed,   // the server world is stopping, or the process is exiting
    // The content worker's lifetime guard. ShroudForge's ResourceLifeGuard answers a bare false
    // (hold_resource_lifetime.cpp:60-123), so these names are Kiln's, in its style, one per refusal; where the guard
    // makes a check ShroudForge's engine context names elsewhere it answers that name instead (ecs-thread-registry-busy,
    // -count-invalid, -range-invalid, -row-unreadable, ecs-thread-name-unreadable, the owner resolution's reasons, and
    // the gate's three kiln-* thread refusals).
    ResourceLifetimeAlreadyHeld,        // the record is held: never touch its word (Prepare/AcquirePrepared :65,87)
    ResourceLifetimeUnavailable,        // no exact profile, or its resourceWriteLayout is not enabled (Prepare :71)
    ResourceLifetimeGateNotHeld,        // borrowed: the gate record is not held (TryAcquire(scene) :81)
    ResourceLifetimeGateChanged,        // borrowed: the held gate no longer validates (:81, scene.Validate())
    ResourceLifetimeRegistryMismatch,   // borrowed: the gate's registry word or view is not this image's (:93)
    ResourceLifetimeWorkerIsAmbiguous,  // a second registered content worker row (:110)
    ResourceLifetimeWorkerRangeInvalid, // the worker row's handle or thread address is invalid (:110)
    ResourceLifetimeWorkerUnregistered, // no registered content worker (:116)
    ResourceLifetimeServiceMismatch,    // the registered worker's service is not the caller's (:116)
    ResourceLifetimePayloadUnavailable, // the service's payload pointer or size is unreadable or zero (:118-119)
    ResourceLifetimePayloadRangeInvalid,  // payload + payload_size overflows (:120)
    // Validate (:36-47), at acquire and on every validate:
    ResourceLifetimeWorkerInvalidOrStopping,  // the worker thread's name, handle, callback, context or stop flag
    ResourceLifetimeServiceNotInitialized,    // the service's initialized flag
    ResourceLifetimeReaderChanged,            // the service's reader is not the caller's
    ResourceLifetimePayloadChanged,           // the service's payload pointer or size changed
    ResourceLifetimeOwnerChanged,             // the owners no longer resolve, or resolve to others
    // An engine call inside a held gate. Kiln's names: ShroudForge's spawn answers its own result codes
    // (impact/submit_building_prop_spawn.cpp:776-885). Refused, nothing called:
    EngineCallGateNotHeld,     // no held gate record
    EngineCallLatched,         // an earlier call of this server session faulted or was not proven; cleared by STARTED
    EngineCallRecipeUnavailable,  // no exact profile, or no well-formed recipe with that id on this build
    EngineCallArgumentInvalid,    // argument count, unused argument, capture size or a buffer's range is wrong
    EngineCallArgumentOnStack,    // a buffer or the capture lies on the calling thread's stack (or its range is unknown)
    EngineCallGateInvalid,        // the held gate no longer validates
    EngineCodeChanged,            // the live code differs from the caller's capture (validate_code before the call)
    // Uncertain, latched:
    EngineCallFaulted,                // an exception reached the call's frame (SEH)
    EngineCallCodeChangedDuringCall,  // the live code differs from the capture after the call
    EngineCallGateChangedDuringCall,  // the gate no longer validates after the call
    EngineCallMemberUncertain,        // the member's own check after the call failed (op Latch)
    // Refused, nothing called (appended so the values above keep their numbers):
    EngineCallTarget,  // the gate's image is not the loaded game image (mem::image_base()): the call target is not its code
    Count
};

inline const char* reason_name(Reason r) {
    static constexpr const char* names[] = {
        "",
        "engine-owner-profile-unavailable", "ecs-read-owner-profile-unavailable",
        "dedicated-server-has-no-local-client-input",
        "ecs-active-scene-list-unavailable", "ecs-active-scene-list-range-invalid", "ecs-scene-state-unreadable",
        "ecs-local-authority-is-ambiguous", "ecs-ready-scene-context-invalid", "ecs-active-scene-is-transitioning",
        "ecs-ready-scene-context-changed", "ecs-active-scene-list-changed",
        "ecs-thread-registry-busy", "ecs-server-world-unavailable", "root-vtable-mismatch",
        "client-owner-unavailable-or-vtable-mismatch", "client-controller-unavailable-or-vtable-mismatch",
        "client-controller-owner-mismatch", "main-owner-unavailable",
        "ecs-server-world-owner-changed", "ecs-execution-owner-unavailable", "ecs-manager-registry-owner-mismatch",
        "ecs-entity-table-unavailable", "ecs-entity-table-range-overflow", "ecs-engine-owner-changed-during-read",
        "ecs-server-thread-layout-invalid", "ecs-thread-registry-count-invalid", "ecs-thread-registry-range-invalid",
        "ecs-thread-registry-row-unreadable", "ecs-thread-name-unreadable", "ecs-server-thread-is-ambiguous",
        "ecs-server-thread-range-invalid", "ecs-server-thread-unregistered", "ecs-server-thread-owner-invalid-or-stopping",
        "ecs-server-thread-changed-during-read",
        "ecs-write-guard-already-held", "ecs-write-gate-disabled", "ecs-write-requires-current-local-authority",
        "ecs-scene-frame-busy", "ecs-write-owner-changed-or-not-authoritative", "ecs-change-tracker-context-invalid",
        "kiln-in-frame", "kiln-world-thread", "kiln-caller-thread-unknown", "kiln-gate-busy", "kiln-gate-busy-this-step",
        "kiln-gate-closed",
        "resource-lifetime-guard-already-held", "resource-lifetime-unavailable", "resource-lifetime-gate-not-held",
        "resource-lifetime-gate-changed", "resource-lifetime-registry-mismatch", "resource-lifetime-worker-is-ambiguous",
        "resource-lifetime-worker-range-invalid", "resource-lifetime-worker-unregistered",
        "resource-lifetime-service-mismatch", "resource-lifetime-payload-unavailable",
        "resource-lifetime-payload-range-invalid", "resource-lifetime-worker-invalid-or-stopping",
        "resource-lifetime-service-not-initialized", "resource-lifetime-reader-changed",
        "resource-lifetime-payload-changed", "resource-lifetime-owner-changed",
        "engine-call-gate-not-held", "engine-call-latched", "engine-call-recipe-unavailable",
        "engine-call-argument-invalid", "engine-call-argument-on-stack", "engine-call-gate-invalid", "engine-code-changed",
        "engine-call-faulted", "engine-call-code-changed-during-call", "engine-call-gate-changed-during-call",
        "engine-call-member-uncertain", "engine-call-target",
    };
    static_assert(sizeof names / sizeof names[0] == static_cast<std::size_t>(Reason::Count), "one name per reason");
    const auto i = static_cast<std::uint32_t>(r);
    return i < static_cast<std::uint32_t>(Reason::Count) ? names[i] : "unknown";
}

}  // namespace kiln::engine
