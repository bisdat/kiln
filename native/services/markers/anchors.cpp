// SPDX-License-Identifier: MIT
// kiln.markers S3, following an entity (chapter 04 §4.2): not built yet. Every member answers unavailable.
#include "glue.h"

namespace kmk {

void anchors_start() {}
void anchors_ready() {
    static const char kNotBuilt[] = "not built yet in this kiln.markers";
    report("markers.anchors.client", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY, kNotBuilt);
    report("markers.anchors.server", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY, kNotBuilt);
}
void anchors_world(const KilnWorldEvent&) {}
void anchors_post(uint32_t) {}
KilnResult anchors_capture(KilnOwner, uint32_t, uint64_t, KilnEntity, KilnMarkerAnchorResult*) { return KILN_ERR_UNAVAILABLE; }
KilnResult anchors_release(KilnOwner, KilnMarkerAnchor) { return KILN_ERR_UNAVAILABLE; }
void anchors_release_owner(KilnOwner) {}
bool anchors_follow(uint64_t, KilnVec3*, uint32_t*) { return false; }

}  // namespace kmk
