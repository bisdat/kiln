// SPDX-License-Identifier: MIT
// kiln.decisions: the SEH guard around a hook's body (death.cpp, pickup.cpp). Its own translation unit: a function with
// __try may hold no C++ object with a destructor. A fault (or anything thrown) in the body is caught here, on the
// hook's thread, before the core's hook chain disables the link: the producer then shuts the service down (shutdown.h)
// and the visit goes on as without Kiln. The bodies change registers only at their very end, so a caught fault leaves
// the game's registers as they were.
#include <windows.h>

#include "services/kiln_hooks.h"

namespace kiln::decisions::seh {

bool counter(uintptr_t address, uint32_t* out) {
    __try { *out = *reinterpret_cast<const volatile uint32_t*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool call(void (*body)(KilnRegisters& r), KilnRegisters& r, unsigned long* code) {
    __try {
        body(r);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace kiln::decisions::seh
