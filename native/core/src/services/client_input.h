// SPDX-License-Identifier: MIT
// kiln.client's local input selection: the one place Kiln decides which keen::ecs::ClientPlayerInput is the local
// player's, and the one entry of its observation on the client's main thread. Every reader and writer of the local input
// goes through it (the building-input producer, client_building_input.h, now; a local input sample
// later), so each engine function on the input path gets at most one detour, owned here.
//
// The selection today: the local player's entity (client.cpp reads LocalPlayerData at each client-world Pre) and the
// client session it was read in. The observation point is a detour in the client cursor system, where the frame's
// ClientPlayerInput is complete and the building systems have not read it yet (ShroudForge 1.7.9 hooks the same system).
// No supported build has that point, the game's building version call or the ClientPlayerInput layout in sigdb, so
// nothing calls observe() and the building-input producer stays unbound (building_input::bound() says why). The change
// that verifies them installs the detour here, calls observe() from it and binds the producer with the build's layout.
#pragma once
#include <cstdint>

#include "client_building_input.h"

namespace kiln::client_input {

struct Selection {
    KilnEntity entity;  // the local player's entity, 0 = none
    uint32_t session;   // the client session (KilnSessionInfo.sessions[KILN_WORLD_CLIENT]) it belongs to, 0 = none
};
void publish(KilnEntity entity, uint32_t session);  // client.cpp, on the client's main thread
Selection selection();                              // any thread: one consistent copy

// The observation's entry, on the client's main thread: one ClientPlayerInput the cursor system processes, its entity
// and the game's building version for the visit's execution view. Only the selected (local) player's input goes on to
// the producer, through SEH-guarded copies.
void observe(uintptr_t record, KilnEntity entity, uint32_t version);

// Copies of game memory under a structured-exception guard (guard::copy): a fault answers false.
building_input::Memory guarded_memory();

}  // namespace kiln::client_input
