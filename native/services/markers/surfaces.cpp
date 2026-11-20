// SPDX-License-Identifier: MIT
// kiln.markers S5, S6, the surfaces (chapter 04 §4.5, §4.6): not built yet.
#include "glue.h"

namespace kmk {

void surfaces_ready() {
    static const char kNotBuilt[] = "not built yet in this kiln.markers";
    for (const char* name : {"markers.map", "markers.compass", "markers.world", "markers.nameplates"})
        report(name, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY, kNotBuilt);
}
void surfaces_world(const KilnWorldEvent&, std::vector<Note>*) {}
void surfaces_client_post(std::vector<Note>*) {}

}  // namespace kmk
