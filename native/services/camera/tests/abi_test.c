/* SPDX-License-Identifier: MIT
 * kiln.camera's header as C: it compiles alone and twice, its layout is abi_asserts.h's, and a field-of-view-only
 * request can be written without a pose or a profile. run.sh builds and runs it. */
#include "services/kiln_camera.h"
#include "services/kiln_camera.h" /* the include guard */
#include "../abi_asserts.h"
#include <stdio.h>

static void KILN_CALL on_done(const KilnCameraResult* r, void* user) { (void)r, (void)user; }
static void KILN_CALL on_changed(const KilnCameraChange* c, void* user) { (void)c, (void)user; }

int main(void) {
    KilnCameraDone done = &on_done;
    KilnCameraChanged changed = &on_changed;
    KilnCameraRequest lens = {0};
    lens.size = sizeof lens;
    lens.parts = KILN_CAMERA_LENS;
    lens.endpoint = KILN_CAMERA_LOCAL_VISUAL;
    lens.input_policy = KILN_CAMERA_VISUAL_ONLY;
    lens.duration_ms = 10000;
    lens.lens_source = KILN_CAMERA_LENS_CONSTANT;
    lens.fov_y_degrees = 40;
    if (!done || !changed || lens.keys || lens.key_count || lens.parts != 16) return 1;
    printf("camera abi: PASS (sizes, offsets, enum values, C callbacks, a lens-only request)\n");
    return 0;
}
