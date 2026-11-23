// SPDX-License-Identifier: MIT
// kiln.ui.snapshot inside kiln-core (abi/services/kiln_ui_snapshot.h is the public table): what its two files share.
//
//   ui_snapshot.cpp          demands, tokens, the collector (client-world Post, the 05a stage, the hosted server's
//                            Post) and the publications: plain C++ over Env below
//   ui_snapshot_service.cpp  the production environment (kiln.client, GetEntityTransform under an SEH guard, kiln.time,
//                            the 05a stage, kiln.caps, the owner registry), the tick subscriptions and the registration
//
// native/core/tests/ui_snapshot_test.cpp builds ui_snapshot.cpp with the host compiler against stub worlds.
#pragma once
#include <cstddef>
#include <cstdint>

#include "services/kiln_ui_snapshot.h"
#include "ui_values_internal.h"  // the pre-provider stage (Stage, StageFn)

namespace kiln::uisnapshot {

// ---- limits (chapter 09 §4.4, §5.1) ----
inline constexpr uint64_t kCadenceMs = 100;     // at most one collection per 100 ms
inline constexpr uint64_t kMaxAgeMs = 1000;     // older fields are refused
inline constexpr uint64_t kTokenTtlMs = 2000;   // a token's pin
inline constexpr uint64_t kMatchGraceMs = 250;  // how long a position waits for the stage of its step
inline constexpr uint32_t kTokensPerOwner = 2, kTokensGlobal = 128;
inline constexpr uint32_t kDemandsPerOwner = 64, kDemandsGlobal = 1024;
inline constexpr uint32_t kRowsMax = 512;
inline constexpr uint32_t kKnownMask = KUW_LOCAL_VITALS | KUW_LOCAL_POSITION | KUW_LOCAL_INVENTORY | KUW_HOST_TIME;
inline constexpr size_t kTransformBytes = 56;  // keen::ecs::CurrentTransform (= keen::WorldTransform)

// What runs in this process now (any thread).
struct WorldNow {
    uint32_t state = 0;           // KILN_SESSION_*
    uint32_t client_session = 0;  // client worlds since boot (kiln.client)
    uint32_t server_session = 0;  // server worlds since boot
    uint32_t local_player = 0;    // kiln.client's local player, 0 = none
    uintptr_t client_world = 0;   // the client world (dereferenced only on its own thread)
};

// A read of the game: done, absent (the game answered "none"), or not possible (faulted, no reader, no service).
enum : int { kReadAbsent = 0, kReadOk = 1, kReadFault = -1 };

// ---- what the service takes from its host (production: ui_snapshot_service.cpp; host tests: stubs) ----
struct Env {
    uint64_t (*tick_ms)() = nullptr;  // GetTickCount64
    bool (*client_side)() = nullptr;
    bool (*owner_alive)(KilnOwner) = nullptr;
    bool (*now)(WorldNow* out) = nullptr;
    // GetEntityTransform(world, entity) on the client world's thread with the 56 bytes copied to out: kReadOk, kReadAbsent
    // (null: no transform), kReadFault (a structured exception: the reader is switched off for the process).
    // Null = the symbol did not resolve on this build.
    int (*read_transform)(uintptr_t world, uint32_t entity, uint8_t* out) = nullptr;
    // kiln.time's get on the hosted server world's thread: kReadOk + hours, kReadAbsent (refused: no world yet),
    // kReadFault (kiln.time is not here).
    int (*host_hours)(double* out) = nullptr;
    KilnResult (*stage_subscribe)(uivalues::StageFn fn, void* user, KilnHandle* out) = nullptr;
    KilnResult (*stage_unsubscribe)(KilnHandle h) = nullptr;
    void (*row_off)(const char* name, uint32_t reason, const char* text) = nullptr;  // KILN_CAP_REASON_*
    void (*log)(int level, const char* text) = nullptr;
};
Env& env();

const KilnUiSnapshotApi* table();

// ---- the collector's entry points ----
void on_client_post(uint64_t step);  // the client world's Post (main thread)
void on_server_post(uint64_t step);  // the server world's Post (its tick thread; a hosting client only)
void on_world_event(uint32_t event, uint32_t kind, uint32_t session);  // KILN_WORLD_STARTED / STOPPING
void owner_retired(KilnOwner owner);
void stage_observer(const uivalues::Stage* stage, void* user);  // 05a's pre-provider stage (the update's thread)

// Counters for the host test and the log.
struct Stats {
    uint64_t transform_reads = 0, time_reads = 0, stage_copies = 0, publications = 0, stage_subscribes = 0,
             stage_unsubscribes = 0;
};
Stats stats();
uint32_t demand_union();
void reset_for_tests();

}  // namespace kiln::uisnapshot
