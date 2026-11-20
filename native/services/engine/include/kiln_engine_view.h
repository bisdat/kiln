// SPDX-License-Identifier: MIT
// kiln.internal.engine records: the owner/Scene/ECS view and the reasons a view or a gate is refused. Kiln-internal
// (see kiln_engine_native.h). Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer),
// modules/game-runtime/native/engine-context/engine_owners.h:9-56 (EcsRole, Owners, EcsView); the reason names are
// ShroudForge's own refusal strings (the fail("...") literals of engine-context/*.cpp), one Kiln code each.
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
// calling KilnOwner, `step` = the server step the caller observed (KilnTickInfo.step), both nonzero, the rest zero;
// release keeps size, owner and step, so a record can be reused at a later step. Never edit it while held.
// `maximum_bytes` is the profile's write budget per component (the member refuses larger writes with
// ecs-write-size-exceeds-gate-budget). `holder`, `boosted` and `saved_priority` are the gate's own. `admission` is
// the caller's admission mode (GateAdmission, set before acquiring, kept by release).
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
};

// Who is asking, for Kiln's own admission (EcsGate.admission):
// - KilnMod (the default): a Kiln mod or task; any busy refusal latches its owner for the rest of the server step;
// - RetryKilnHolder: never latched; a refusal caused only by another Kiln holder is retried up to 128 times with no
//   pause, before any engine word is touched (never against an engine exclusive holder). For callers that schedule
//   their own retries and must not be held back for the rest of a step.
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
    };
    static_assert(sizeof names / sizeof names[0] == static_cast<std::size_t>(Reason::Count), "one name per reason");
    const auto i = static_cast<std::uint32_t>(r);
    return i < static_cast<std::uint32_t>(Reason::Count) ? names[i] : "unknown";
}

}  // namespace kiln::engine
