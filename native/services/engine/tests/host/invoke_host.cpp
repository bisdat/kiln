// SPDX-License-Identifier: MIT
// Host bodies of invoke.h's call functions for tests (the Windows ones are invoke_win.cpp): the "engine call" is a test
// hook, since the fake image holds no code (Wine and native Windows run real helper code: tests/windows_host.cpp); the
// compare reads the live bytes through process_vm_readv in chunks, so an unmapped page fails it as the SEH compare does
// on Windows (and the kernel copy is invisible to ThreadSanitizer, like the engine's racy-by-design reads); the stack
// range is pthread's, read once per thread.
#include <pthread.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cstring>

#include "invoke.h"

namespace kiln::engine::call {
// The test's stand-in for the engine function: what it returns, whether it "faulted", and anything it does to engine
// memory meanwhile. No hook: returns 0.
std::atomic<Return (*)(std::uintptr_t entry, const std::uint64_t* args)> test_call_hook{nullptr};
std::atomic<std::uint64_t> test_calls{0}, test_compares{0};
std::atomic<bool> test_stack_unknown{false};  // stack_range cannot tell

Return invoke(std::uintptr_t entry, const std::uint64_t* args) noexcept {
    test_calls.fetch_add(1, std::memory_order_relaxed);
    if (const auto hook = test_call_hook.load(std::memory_order_acquire)) return hook(entry, args);
    return {0, 0, 0};
}

bool same(std::uintptr_t address, const std::uint8_t* expected, std::size_t size) noexcept {
    test_compares.fetch_add(1, std::memory_order_relaxed);
    if (!address || address > UINTPTR_MAX - size) return false;
    std::uint8_t chunk[256];
    for (std::size_t done = 0; done < size;) {
        const std::size_t n = size - done < sizeof chunk ? size - done : sizeof chunk;
        iovec local{chunk, n}, remote{reinterpret_cast<void*>(address + done), n};
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != static_cast<ssize_t>(n)) return false;
        if (std::memcmp(chunk, expected + done, n) != 0) return false;
        done += n;
    }
    return true;
}

namespace {
thread_local std::uintptr_t t_low = 0, t_high = 0;
}
bool stack_range(std::uintptr_t& low, std::uintptr_t& high) noexcept {
    if (test_stack_unknown.load(std::memory_order_relaxed)) return false;
    if (!t_high) {
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) != 0) return false;
        void* base = nullptr;
        std::size_t size = 0;
        const bool ok = pthread_attr_getstack(&attr, &base, &size) == 0;
        pthread_attr_destroy(&attr);
        if (!ok || !base || !size) return false;
        t_low = reinterpret_cast<std::uintptr_t>(base);
        t_high = t_low + size;
    }
    low = t_low;
    high = t_high;
    return true;
}
}  // namespace kiln::engine::call
