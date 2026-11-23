// SPDX-License-Identifier: MIT
// Loader fixture only: build both DLLs as kiln-rules.dll in separate directories.
#include "kiln_api.h"

namespace {
using ExactFn = bool(KILN_CALL*)(const void*);
ExactFn exact = nullptr;
}

#ifndef HISTORY_BRIDGE_MISSING
extern "C" KILN_EXPORT void KILN_CALL kiln_rules_exact_history(ExactFn predicate) { exact = predicate; }
#endif
extern "C" KILN_EXPORT bool KILN_CALL history_bridge_exact(const void* materials) {
    return exact && exact(materials);
}
