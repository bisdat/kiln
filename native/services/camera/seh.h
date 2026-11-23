// SPDX-License-Identifier: MIT
#ifndef KILN_CAMERA_SEH_H
#define KILN_CAMERA_SEH_H
#include "services/kiln_camera.h"

namespace kcam {
// A callback into another owner's code, under a structured-exception guard. False: it faulted.
bool seh_done(KilnCameraDone fn, const KilnCameraResult* result, void* user);
bool seh_changed(KilnCameraChanged fn, const KilnCameraChange* change, void* user);
}  // namespace kcam
#endif
