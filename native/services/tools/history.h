// SPDX-License-Identifier: MIT
#pragma once
#include "kiln_api.h"

// Private, native-only integration. The tools DLL owns the scoped policy and the history tags; the rules DLL merely
// asks whether a native material-list descriptor belongs to a policy history entry. No game pointer reaches mods.
bool kiln_tools_history_install(const KilnApi* api, KilnOwner owner);
// Global service shutdown only, after impact admission is closed/drained and native undo/world jobs are quiesced.
// Never call on mod unregister: existing game history must retain its exact-refund guard until it can no longer run.
void kiln_tools_history_uninstall();
extern "C" KILN_EXPORT bool KILN_CALL kiln_tools_history_exact(const void* materials);
