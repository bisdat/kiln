// SPDX-License-Identifier: MIT
// The kiln.tasks pool over the REAL Windows platform under headless Wine (test-tasks-wine.sh): bodies in
// a loaded DLL (tasks_test_module.dll) run on worker threads, the module stays referenced (FreeLibrary does not unmap
// it) while a task is in flight and is unloaded once done returned (the pool released every reference), a native fault in an exported body is contained by
// the SEH boundary and quarantines its owner, and a stack-overflow body is contained or its inability documented.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "tasks.h"

using namespace kiln::tasks;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}
template <class F>
bool until(F pred, int ms = 5000) {
    for (int i = 0; i < ms && !pred(); ++i) Sleep(1);
    return pred();
}
// mirrors tasks_test_module.cpp's Shared
struct Shared {
    volatile long body_runs = 0;
    volatile long done_calls = 0;
    volatile unsigned done_state = 0;
    volatile int* release_flag = nullptr;
    volatile long cancel_seen = 0;
};
SubmitArgs named(const char* n, bool long_running = false) {
    SubmitArgs a;
    a.name = n;
    a.long_running = long_running;
    return a;
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: nothing is lost if a later stage hangs
    std::printf("tasks windows host: start\n");
    HMODULE module = LoadLibraryW(L"tasks_test_module.dll");
    check(module != nullptr, "the test module loads");
    if (!module) return 1;
    TaskBodyFn body = nullptr, fault_body = nullptr, stack_body = nullptr;
    TaskDoneFn done = nullptr;
    const auto resolve = [&] {
        body = reinterpret_cast<TaskBodyFn>(GetProcAddress(module, "kiln_test_body"));
        done = reinterpret_cast<TaskDoneFn>(GetProcAddress(module, "kiln_test_done"));
        fault_body = reinterpret_cast<TaskBodyFn>(GetProcAddress(module, "kiln_test_fault_body"));
        stack_body = reinterpret_cast<TaskBodyFn>(GetProcAddress(module, "kiln_test_stack_body"));
        return body && done && fault_body && stack_body;
    };
    check(resolve(), "exports found");
    if (!body) return 1;

    PoolConfig config;
    config.normal_workers = 2;
    config.long_running_workers = 2;
    config.limits.owner_capacity = 32;
    Pool pool(config);  // the real WindowsPlatform (default_platform)
    pool.open_owner(1, "mod.one");

    // a cooperative task: the body runs in the module and the module stays mapped until done returns
    int release = 0;
    Shared shared;
    shared.release_flag = &release;
    KilnHandle h = 0;
    Refusal why = Refusal::None;
    check(pool.submit(1, named("work"), body, done, &shared, &h, &why) == KILN_OK, "submit a module task");
    check(until([&] { return shared.body_runs == 1; }), "the body ran in the module");
    // FreeLibrary once: our acquire plus the pool's reference keep it mapped while the task is in flight
    FreeLibrary(module);
    HMODULE still = nullptr;  // a probe that takes no reference of its own
    check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(body), &still) &&
              still,
          "the module stays mapped while the task is in flight (the pool holds a reference)");
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&release), 1);
    check(until([&] { return shared.done_calls == 1; }), "done ran");
    check(shared.done_state == static_cast<unsigned>(State::Completed), "done saw Completed");
    uint32_t out = 1;
    pool.drain_owner(1, 0, &out);
    check(until([&] {
        uint32_t left = 1;
        pool.drain_owner(1, 0, &left);
        return left == 0;
    }), "the task reached quiescence");
    pool.release(1, h);
    // the pool dropped its references just after done returned: nothing else holds the module, so it is unloaded
    check(until([&] { return GetModuleHandleW(L"tasks_test_module.dll") == nullptr; }),
          "the module unloads after quiescence (every pool reference was released)");
    module = LoadLibraryW(L"tasks_test_module.dll");  // again, for the cases below
    check(module != nullptr && resolve(), "the test module loads again");
    if (!module || !body) return 1;

    // a native fault in an exported body is contained and quarantines the owner
    pool.open_owner(2, "mod.fault");
    Shared fshared;
    KilnHandle fh = 0;
    check(pool.submit(2, named("faulter"), fault_body, done, &fshared, &fh, &why) == KILN_OK, "submit a faulting task");
    check(until([&] {
        State s = State::None;
        pool.status(2, fh, &s);
        return s == State::Faulted;
    }), "the native fault was contained; the task is Faulted");
    KilnHandle after = 0;
    check(pool.submit(2, named("after"), body, done, &fshared, &after, &why) == KILN_ERR_UNAVAILABLE &&
              why == Refusal::OwnerQuarantined,
          "the faulting owner is quarantined");
    // a healthy owner still runs (the faulted worker was retired and replaced)
    pool.open_owner(3, "mod.healthy");
    int release3 = 1;
    Shared hs;
    hs.release_flag = &release3;
    KilnHandle hh = 0;
    check(pool.submit(3, named("healthy"), body, done, &hs, &hh, &why) == KILN_OK, "submit after the fault");
    check(until([&] {
        State s = State::None;
        pool.status(3, hh, &s);
        return s == State::Completed;
    }), "a healthy owner keeps running after the fault, on a replacement worker");

    // a stack-overflow body: contained (quarantines its owner) or, if the platform cannot, that is reported not hung
    pool.open_owner(4, "mod.stack");
    Shared ss;
    KilnHandle sh = 0;
    check(pool.submit(4, named("stack"), stack_body, done, &ss, &sh, &why) == KILN_OK, "submit a stack-overflow task");
    const bool contained = until(
        [&] {
            State s = State::None;
            pool.status(4, sh, &s);
            return s == State::Faulted;
        },
        8000);
    std::printf("stack overflow: %s\n", contained ? "contained (owner quarantined)" : "NOT contained on this platform");
    check(true, "the stack-overflow task did not hang the pool");  // the pool is still responsive below

    // the pool is still responsive: a fresh owner completes a task
    pool.open_owner(5, "mod.after");
    int release5 = 1;
    Shared as;
    as.release_flag = &release5;
    KilnHandle ah = 0;
    pool.submit(5, named("after-stack"), body, done, &as, &ah, &why);
    check(until(
              [&] {
                  State s = State::None;
                  pool.status(5, ah, &s);
                  return s == State::Completed;
              },
              8000),
          "the pool stays responsive after the stack-overflow task");

    std::printf("tasks windows host: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
