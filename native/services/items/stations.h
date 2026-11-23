// SPDX-License-Identifier: MIT
// kiln.stations (since 0.2.0-dev.29): the production stations of a base and the game's own station actions, in
// kiln-items.dll. ABI: native/core/abi/services/kiln_stations.h; evidence: docs/re/stations.md; logic without game memory
// in stations_logic.h (host test tests/stations_test.cpp).
#pragma once
#include <cstdint>
#include "craft_sources.h"

#include "kiln_api.h"
#include "services/kiln_world.h"

namespace kitems {

// Registered linked storage, shared by crafting and dye payment. Caller verifies its transaction inventory view.
std::vector<CraftSource> craft_sources_copy(uint32_t player);
bool craft_source_access(void* server_bases, uint32_t player, uint32_t container);
bool craft_source_access(uint32_t player, uint32_t container);
void craft_sources_cancel_owner(KilnOwner owner);

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

// kiln.items.inventory_kinds (GitHub issue #138), tick thread. Every base's crafting-stock containers as the game lists
// them this frame, sorted and unique; false when the game's bases cannot be read here (stations.read off, no server world).
bool stations_base_stock(std::vector<KilnEntity>* out);
// The entity carries keen::ecs::FactoryStation (a production station as kiln.stations reads it).
bool stations_is_factory(KilnEntity e);

}  // namespace kitems
