// SPDX-License-Identifier: MIT
// A subscriber's callback under a structured-exception guard (Windows): a fault unsubscribes that subscriber and nothing
// else. No frame here holds a C++ object with a destructor, as __try requires. The host build catches a C++ exception
// instead, so the tests can fault a callback.
#ifdef _WIN32
#include <windows.h>
#endif

#include "events.h"

namespace kmk {

bool guarded_call(void (*fn)(void*), void* ctx) {
#ifdef _WIN32
    __try {
        fn(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    try {  // the host tests' stand-in for a fault: a callback that throws
        fn(ctx);
        return true;
    } catch (...) {
        return false;
    }
#endif
}

}  // namespace kmk
