// SPDX-License-Identifier: MIT
// kiln.gemcraft (gem wave G3): the Shroud Forge's gem actions for code and the players' forge actions before and after,
// in kiln-items.dll. ABI: native/core/abi/services/kiln_gemcraft.h; evidence: docs/re/gems-forge.md.
#pragma once
#include <cstdint>

#include "kiln_api.h"
#include "services/kiln_world.h"

namespace kitems {

// resolve(): the feature gems.craft's symbols and constants (after the transaction symbols).
void gems_resolve();
// on_ready: registers kiln_items_gems through kiln.world (code actions).
void gems_register(const KilnWorldApi* w);
// Tick thread, Pre: the layout check against equipment_actions' declaration, results, timeouts, the hold lease.
void gems_pre(uint64_t step);
// The server world closed: waiting code actions end DROPPED, held players' actions are forgotten.
void gems_world_closed();
// kiln_plugin_main: publishes kiln.gemcraft (also when gems.craft is off: status says why).
KilnResult gems_publish(const KilnApi* api, KilnOwner owner);

}  // namespace kitems
