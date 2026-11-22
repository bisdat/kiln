// SPDX-License-Identifier: MIT
// kiln.repl.net (native/core/abi/services/kiln_repl.h, since 0.2.0-dev.31): the table, handle routing and the lifecycle
// fan-out to its parts (repl.h "kiln.repl.net"). The dossier's chapter 15; DESIGN: out/15-replication-sidecars/DESIGN.md.
#include "repl.h"

namespace repl {

namespace {

uint32_t KILN_CALL t_net_capabilities() { return net_capabilities(); }

KilnResult KILN_CALL t_net_remove(KilnHandle h) {
    switch (h & kTagMask) {
    case kTrackTag: return track_remove(h);
    case kFilterTag: return filter_remove(h);
    case kMessageTag: return messages_remove(h);
    default: return h ? KILN_ERR_NOT_FOUND : KILN_ERR_INVALID_ARGUMENT;
    }
}

KilnResult KILN_CALL t_net_peers(uint32_t* out_peers, uint8_t* out_flags, uint32_t cap, uint32_t* count) {
    if (count) *count = KILN_REPL_MACHINES;
    if (cap && (!out_peers || !out_flags)) return KILN_ERR_INVALID_ARGUMENT;
    for (uint32_t m = 0; m < cap && m < KILN_REPL_MACHINES; ++m) {
        uint8_t flags = 0;
        out_peers[m] = peer_of(m, &flags);
        out_flags[m] = flags;
    }
    return KILN_OK;
}

}  // namespace

const KilnReplNetApi kNetTable = {{sizeof(KilnReplNetApi), KILN_REPL_NET_VERSION},
                                  &t_net_capabilities,
                                  &t_net_remove,
                                  &t_track,
                                  &t_peer_entities,
                                  &t_filter_add,
                                  &t_filter_set_peers,
                                  &t_touch,
                                  &t_remap_set,
                                  &t_remap_peers,
                                  &t_remap_stats,
                                  &t_message_hook,
                                  &t_message_inject,
                                  &t_schema_find,
                                  &t_schema_fields,
                                  &t_schema_decode,
                                  &t_schema_encode,
                                  &t_net_peers};

uint32_t net_capabilities() { return track_caps() | filter_caps() | messages_caps() | schema_caps(); }

void net_start() {
    track_start();
    filter_start();
    messages_start();
    schema_start();
}

void net_ready() { filter_ready(); }

void net_pre(uintptr_t world) {
    track_pre();
    filter_pre(world);
    messages_pre();
}

void net_peer_changed(uint32_t machine, uint32_t old_peer) {
    track_peer_changed(machine, old_peer);
    filter_peer_changed(machine, old_peer);
}

void net_world_stopping() {
    track_world_stopping();
    filter_world_stopping();
}

void net_owner_retiring(KilnOwner owner) {
    track_owner_retiring(owner);
    filter_owner_retiring(owner);
    messages_owner_retiring(owner);
}

}  // namespace repl
