// SPDX-License-Identifier: MIT
// kiln.repl.net's sidecar tracking (repl.h "track.cpp").
// Not built yet (package 15 F0): every function answers KILN_ERR_UNAVAILABLE and the capability bits stay 0.
#include "repl.h"

namespace repl {

void track_start() {}
uint32_t track_caps() { return 0; }
bool track_active() { return false; }
void track_callback(const Batch*) {}
void track_pre() {}
void track_peer_changed(uint32_t, uint32_t) {}
void track_world_stopping() {}
void track_owner_retiring(KilnOwner) {}
KilnResult track_remove(KilnHandle) { return KILN_ERR_NOT_FOUND; }
KilnResult KILN_CALL t_track(KilnOwner, uint32_t, KilnReplTrackFn, void*, KilnHandle* out) {
    if (out) *out = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL t_peer_entities(uint32_t, KilnReplPeerEntity*, uint32_t, uint32_t* count) {
    if (count) *count = 0;
    return KILN_ERR_UNAVAILABLE;
}

}  // namespace repl
