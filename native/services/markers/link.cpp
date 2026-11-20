// SPDX-License-Identifier: MIT
// kiln.markers S7, S8, the link (chapter 04 §4.4, §4.5): not built yet. Every member answers unavailable naming markers.send.
#include <cstdio>
#include <cstring>

#include "glue.h"

namespace kmk {

namespace {
KilnResult off(KilnMarkerResult* r) {
    if (r && r->size >= sizeof(KilnMarkerResult)) {
        const uint32_t size = r->size;
        std::memset(r, 0, sizeof(KilnMarkerResult));
        r->size = size;
        std::snprintf(r->reason, sizeof r->reason, "markers.send");
    }
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL b_attach(KilnOwner) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL b_peer_up(const KilnMarkerPeer*) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL b_peer_down(uint32_t, uint32_t, uint64_t) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL b_receive(const KilnMarkerEnvelope*) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL b_pump(uint32_t, uint32_t, KilnMarkerSendFn, void*, uint32_t* processed) {
    if (processed) *processed = 0;
    return KILN_ERR_UNAVAILABLE;
}
}  // namespace

void link_ready() {
    report("markers.send", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY, "not built yet in this kiln.markers");
}
void link_world(const KilnWorldEvent&) {}
void link_server_post() {}
void link_client_post() {}
KilnResult link_send(KilnOwner, KilnEntity, const KilnMarkerSpec*, KilnMarkerResult* r) { return off(r); }
KilnResult link_unsend(KilnOwner, KilnEntity, KilnStr, KilnMarkerResult* r) { return off(r); }
KilnResult link_clear_sent(KilnOwner, KilnEntity, KilnMarkerResult* r) { return off(r); }
KilnResult link_suppress_sent(KilnOwner, KilnEntity, KilnStr, KilnMarkerResult* r) { return off(r); }
void link_release_owner(KilnOwner) {}

const KilnMarkersBridgeApi kBridgeTable = {{sizeof(KilnMarkersBridgeApi), KILN_MARKERS_BRIDGE_VERSION},
                                           &b_attach, &b_peer_up, &b_peer_down, &b_receive, &b_pump};

}  // namespace kmk
