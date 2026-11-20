// SPDX-License-Identifier: MIT
// The kiln.tasks fault boundary: run one callback under Windows structured exception handling, so a
// native fault in a mod's body or done is contained instead of taking the process down. Kept in its own file because
// __try/__except may not share a function with C++ objects that unwind; only POD is live across the boundary here.
// It only classifies and contains; the pool logs the fault and quarantines the owner.
#include <windows.h>

#include "tasks_platform.h"

namespace kiln::tasks {
namespace {
int seh_filter(unsigned long code, FaultInfo* info) {
    info->faulted = true;
    info->code = code;
    // A structured fault means the OS thread's state is suspect (a half-updated mod, a sprung guard page): always
    // retire the thread. A stack overflow in particular cannot be safely resumed; the pool retires and replaces it.
    info->thread_tainted = true;
    return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace

// Reserve enough stack for the __except filter to run even on a stack-overflow fault; best effort.
void seh_prepare_thread() {
    ULONG guarantee = 32 * 1024;
    SetThreadStackGuarantee(&guarantee);
}

void guarded_call_seh(GuardedFn fn, void* context, FaultInfo* info) {
    *info = FaultInfo{};
    info->thread_tainted = false;  // an ordinary return taints nothing
    __try {
        fn(context);
    } __except (seh_filter(GetExceptionCode(), info)) {
        // contained; info describes the fault, info->thread_tainted is true
    }
}

}  // namespace kiln::tasks
