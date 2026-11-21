// SPDX-License-Identifier: MIT
// kiln.client's local input selection: the one place Kiln decides which keen::ecs::ClientPlayerInput is the local
// player's, and the one entry of its observation on the client's main thread. Every reader and writer of the local input
// goes through it (the building-input producer, client_building_input.h, now; a local input sample
// later), so each engine function on the input path gets at most one Kiln link, owned here.
//
// The selection: the local player's entity (client.cpp reads LocalPlayerData at each client-world Pre) and the client
// session it was read in. The observation point (sigdb client.input_observation, docs/kiln/services/
// client-building-input.md): the client's local input writer, right after it filled the frame's ClientPlayerInput
// (digitalInput for this client world step, the camera) and before the step's input message copies it; the client
// world's programs (the cursor among them) run after that message. client.cpp installs it as one kiln.hooks mid link,
// which shares its detour with kiln.rules' read-only camera check, and binds the producer with the build's layout when
// the point resolves and the client gate does not hold client.building_input. The link calls visit().
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

// The observation's entry, on the client's main thread: one ClientPlayerInput the input writer filled, its entity and
// the game's building input version for the visit (the client world's step, as the game stamps its own input actions).
// Only the selected (local) player's input goes on to the producer, through SEH-guarded copies.
void observe(uintptr_t record, KilnEntity entity, uint32_t version);

// Where the observation point finds the version (sigdb client.input_observation): the client world's pointer in the
// client game's input object, and the step counter in that world (byte offsets).
struct Site {
    uint32_t world;  // client/input.world
    uint32_t step;   // client/input.step
};
// One visit at the observation point (the link's handler; the client's main thread): `input` is the client game's input
// object, `record` the ClientPlayerInput the writer just filled and `entity` its entity. Reads the step through guarded
// copies (a fault skips the visit and is counted) and goes on to observe().
void visit(uintptr_t input, uintptr_t record, KilnEntity entity, const Site& site);

struct Stats {
    uint64_t visits;  // visits at the observation point
    uint64_t faults;  // visits whose step could not be read (skipped)
};
Stats stats();
void reset_stats_for_tests();

// Copies of game memory under a structured-exception guard (guard::copy): a fault answers false.
building_input::Memory guarded_memory();

}  // namespace kiln::client_input
