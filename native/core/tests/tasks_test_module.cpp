// SPDX-License-Identifier: MIT
// A small DLL for the kiln.tasks Wine test (tasks_windows_host.cpp): its exported callbacks live in a real module, so
// the test can prove the pool keeps the module referenced while a task is in flight, and that a native fault inside an
// exported body is contained. Never a game DLL. The task ABI shape is mirrored privately here (the public header is kiln_tasks.h).
#include <windows.h>

#include <atomic>
#include <cstdint>

namespace {
struct TaskRun {
    uint32_t size, reserved;
    uint64_t task;
    uint64_t owner;
    uint32_t(__stdcall* cancelled)(const TaskRun*);
    void* internal;
};
struct Shared {
    std::atomic<int> body_runs{0};
    std::atomic<int> done_calls{0};
    std::atomic<uint32_t> done_state{0};
    volatile int* release_flag = nullptr;  // the body spins until *release_flag != 0
    std::atomic<int> cancel_seen{0};
};
}  // namespace

extern "C" {

// A cooperative body: counts, optionally waits for a release flag the host sets, and polls cancellation.
__declspec(dllexport) void __stdcall kiln_test_body(const TaskRun* run, void* user) {
    auto* s = static_cast<Shared*>(user);
    s->body_runs.fetch_add(1);
    while (s->release_flag && *s->release_flag == 0) {
        if (run->cancelled && run->cancelled(run)) {
            s->cancel_seen.fetch_add(1);
            return;
        }
        Sleep(1);
    }
}

__declspec(dllexport) void __stdcall kiln_test_done(uint64_t, uint32_t state, void* user) {
    auto* s = static_cast<Shared*>(user);
    s->done_state.store(state);
    s->done_calls.fetch_add(1);
}

// A body that faults: a null write. The pool's SEH boundary must contain it.
__declspec(dllexport) void __stdcall kiln_test_fault_body(const TaskRun*, void*) {
    volatile int* p = nullptr;
    *p = 1;
}

// A body that overflows its stack through unbounded recursion (deliberate; the depth is hidden from the optimiser so
// the warning for guaranteed self-recursion does not fire).
__declspec(dllexport) void __stdcall kiln_test_stack_body(const TaskRun* run, void* user) {
    volatile char buffer[8192];
    buffer[0] = 1;
    static volatile int always = 1;
    if (always) kiln_test_stack_body(run, user);
    (void)buffer;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
