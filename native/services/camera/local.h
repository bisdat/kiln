// SPDX-License-Identifier: MIT
// kiln.camera on a game client: the renderer's two hooks, the final snapshot (get_visual), the render-only camera of
// the local player (the LOCAL_VISUAL backend) and its follow sampler. camera.cpp calls these three and nothing else of
// the client side; on a dedicated server they do nothing.
#ifndef KILN_CAMERA_LOCAL_H
#define KILN_CAMERA_LOCAL_H

#include "kiln_api.h"
#include "services/kiln_camera.h"

namespace kcam {

class Core;

// on_ready, after the core's owner-cleanup decision (core->writes()). Resolves the renderer's symbols and constants,
// installs the hooks, subscribes the client world's lifecycle and gives the core what it can serve.
void local_start(const KilnApi* api, KilnOwner owner, Core* core, bool client_side);
// The client world's Pre, before Core::pre(kClient). True: the local rows changed (report them again).
bool local_pre(const KilnTickInfo* tick);
// The table's get_visual.
KilnResult local_get_visual(KilnCameraVisual* out, uint32_t* out_reason);
// Why the client capability `bit` (a KILN_CAMERA_CAP_LOCAL_* or KILN_CAMERA_CAP_RENDERED_SNAPSHOT) is off here, for its
// row: a KILN_CAP_REASON_* in *reason and the words (valid until the next call of local_start). Null: no reason known
// on this side (a dedicated server, or the bit is on).
const char* local_why(uint64_t bit, uint32_t* reason);

}  // namespace kcam
#endif
