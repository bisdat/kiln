// SPDX-License-Identifier: MIT
// kiln.client's building input: building_input_submit / _status / _cancel and the producer
// behind them (docs/kiln/services/client-building-input.md).
//
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer): modules/game-runtime/native/player-input/
// apply_building_input.cpp of ShroudForge 1.7.9 (the request slot and its states, the transform conversion, and the
// press and release sequence on keen::ecs::ClientPlayerInput). What differs: the layout and the input bits come from the
// build (Layout) instead of constants; a request carries its owner and client session; the producer only try-locks.
//
// One request slot for the whole client. A request is queued, then pressed (or, for a selection, dispatched at once)
// by the producer, then released at a later input version (dispatched); 5 s after submit a queued request times out and
// a pressed one still releases. Ids grow from 1 and skip 0; only the latest id, asked by the owner that submitted it,
// has an answer.
//
// Threads. submit/status/cancel: any thread; they hold the slot's mutex for a copy only, never across a call. The
// producer (observe) runs on the client's main thread inside the local input observation (client_input.h); it only
// try-locks the slot and skips that visit when a caller holds it (stats().skipped), so the game never waits for a mod
// thread. It allocates nothing, logs nothing and calls nothing but the Memory it is given.
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "services/kiln_client.h"

namespace kiln::building_input {

// Where the bound build keeps the parts of one keen::ecs::ClientPlayerInput the producer touches (byte offsets in the
// component), and the bit numbers of its keen::ecs::PlayerInputType.
struct Layout {
    uint32_t cursor;   // data.cursorInput: primaryTransform, then secondaryTransform (56 bytes each)
    uint32_t digital;  // data.digitalInput: Bitmask64<PlayerInputType>
    uint32_t create;   // data.createBuildingItemAction: u32 versionData @0, u8 selectedIndex @4, u32 itemId @8
    uint32_t stock;    // data.buildingStockCycleAction: u32 versionData @0, terrain @4, blueprintMaterialDefault @8
    uint8_t mainhand, mainhand_tap, mainhand_hold, mainhand_release;  // MainhandAction, _Tap, _Hold, _Release
    uint8_t secondary;                                                // SecondaryBuildingAction
    uint8_t contextual, contextual_hold;                              // ContextualAction, ContextualAction_Hold
    uint8_t undo;                                                     // BuildingUndo
};

// The game's memory, copied under a guard: false when the copy faulted.
struct Memory {
    bool (*read)(void* ctx, void* dst, uintptr_t src, size_t n);
    bool (*write)(void* ctx, uintptr_t dst, const void* src, size_t n);
    void* ctx;
};

// One visit of the local input observation to one ClientPlayerInput.
struct Observation {
    uintptr_t record;   // the component's address
    KilnEntity entity;  // its entity, as the local input selection decided
    uint32_t session;   // the client session it belongs to
    uint32_t version;   // the game's building input version for this visit's execution view
};

// The process facts the members check, from the live state (client.cpp).
struct Env {
    bool client_side;       // the game client (a dedicated server has no local input)
    bool hooks;             // not safe idle
    bool owner_alive;       // the calling owner is not revoked
    uint32_t live_session;  // the live client world's session (KilnSessionInfo.sessions[KILN_WORLD_CLIENT]), 0 = none
    // submit asks it again once its request is in the slot (reg::owner_alive; nullptr: never): an owner revoked while
    // its submit ran had owner_gone() find nothing to cancel, so submit cancels that request itself and answers revoked.
    bool (*owner_alive_now)(KilnOwner);
};

KilnResult submit(const Env& env, KilnOwner owner, uint64_t expected_session, const KilnBuildingInput* input,
                  uint32_t* request_id);
KilnResult status(const Env& env, KilnOwner owner, uint32_t request_id, uint32_t* state);
KilnResult cancel(const Env& env, KilnOwner owner, uint32_t request_id, uint32_t* cancelled);

// The producer's binding: the build's layout, given when the local input observation is installed and taken back
// (nullptr) when it is removed. Either way an open request ends cancelled. Nothing binds it on a build without a
// verified observation point; then submit answers KILN_ERR_UNAVAILABLE and bound() says why.
void bind(const Layout* layout);
bool bound(const char** why);
// The producer: one visit (client main thread).
void observe(const Observation& o, const Memory& m);

// The client world's STOPPING (its thread): a request of that session or older ends cancelled, and no later submit binds
// to it.
void session_ended(uint32_t session);
// A revoked owner: its queued request is cancelled, its pressed one still released at the next input.
void owner_gone(KilnOwner owner);

struct Stats {
    uint64_t skipped;  // producer visits skipped because a caller held the slot
    uint64_t faults;   // guarded copies that faulted (the request ended cancelled)
};
Stats stats();

// Host tests.
void set_clock_for_tests(uint64_t (*ms)());  // nullptr: the steady clock
void reset_for_tests();                      // empty slot, ids from 1, unbound, no session ended, zero stats
void set_next_id_for_tests(uint32_t last);   // the next id is last + 1 (skipping 0)
std::mutex& mutex_for_tests();               // the slot's mutex, to hold it from a test

}  // namespace kiln::building_input
