// SPDX-License-Identifier: MIT
// kiln.repl.net's touch (repl.h "touch.cpp").
// Not built yet (package 15 F0): every function answers KILN_ERR_UNAVAILABLE and the capability bits stay 0.
#include "repl.h"

namespace repl {

KilnResult KILN_CALL t_touch(KilnEntity, uint32_t* out_status) {
    if (out_status) *out_status = KILN_REPL_TOUCH_GONE;
    return KILN_ERR_UNAVAILABLE;
}

}  // namespace repl
