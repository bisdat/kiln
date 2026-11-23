// SPDX-License-Identifier: MIT
// No frame here holds a C++ object with a destructor, as __try requires.
#include <windows.h>

#include "seh.h"

namespace kcam {

bool seh_done(KilnCameraDone fn, const KilnCameraResult* result, void* user) {
    __try {
        fn(result, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool seh_changed(KilnCameraChanged fn, const KilnCameraChange* change, void* user) {
    __try {
        fn(change, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace kcam
