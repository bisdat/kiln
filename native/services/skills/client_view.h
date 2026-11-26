// SPDX-License-Identifier: MIT
// kiln.skills.client (since 0.2.0-dev.31.13): the local player's skill tree as the game client holds it, read-only
// (client_view.cpp; ABI native/core/abi/services/kiln_skills_client.h).
#pragma once
#include "kiln_api.h"

namespace kiln::skills::client_view {

// kiln_plugin_main: resolves skills.client_view (a game client only) and publishes kiln.skills.client (also where it is
// off: its state says why).
KilnResult init(const KilnApi* api, KilnOwner owner);
// on_ready: kiln.client (the local player) and the client world's Post tick.
void ready();

}  // namespace kiln::skills::client_view
