// SPDX-License-Identifier: MIT
// kiln.stations (since 0.2.0-dev.29): the production stations of a base and the game's own station actions, in
// kiln-items.dll. ABI: native/core/abi/services/kiln_stations.h; evidence: docs/re/stations.md; logic without game memory
// in stations_logic.h (host test tests/stations_test.cpp).
#pragma once
#include <cstdint>

#include "kiln_api.h"
#include "services/kiln_world.h"

namespace kitems {

// resolve(): the features stations.* (symbols, constants), after the transaction symbols.
void stations_resolve();
// on_ready: registers kiln_items_stations through kiln.world; finds kiln.knowledge, kiln.players, kiln.gameplay.
void stations_register(const KilnWorldApi* w);
// Tick thread, Pre: unlock checks, timeouts, results, the watch (state diff, scans).
void stations_pre(uint64_t step);
// The server world closed: waiting actions end DROPPED, caches and watches are cleared.
void stations_world_closed();
// kiln_plugin_main: publishes kiln.stations (also when its features are off: status says why).
KilnResult stations_publish(const KilnApi* api, KilnOwner owner);

}  // namespace kitems
