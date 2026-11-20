// SPDX-License-Identifier: MIT
// kiln.repl.net's per-peer snapshot filters (repl.h "filter.cpp").
// Not built yet (package 15 F0): every function answers KILN_ERR_UNAVAILABLE and the capability bits stay 0.
#include "repl.h"

namespace repl {

void filter_start() {}
void filter_ready() {}
uint32_t filter_caps() { return 0; }
void filter_pre(uintptr_t) {}
void filter_peer_changed(uint32_t, uint32_t) {}
void filter_world_stopping() {}
void filter_owner_retiring(KilnOwner) {}
KilnResult filter_remove(KilnHandle) { return KILN_ERR_NOT_FOUND; }
KilnResult KILN_CALL t_filter_add(KilnOwner, uint32_t, const uint32_t*, uint32_t, uint32_t, int32_t, const uint32_t*, uint32_t,
                                  KilnReplFilterFn, void*, KilnHandle* out) {
    if (out) *out = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL t_filter_set_peers(KilnHandle, const uint32_t*, uint32_t) { return KILN_ERR_UNAVAILABLE; }

}  // namespace repl
