// SPDX-License-Identifier: MIT
// Host bodies of memory.h for tests: reads go through process_vm_readv (an unmapped page fails the copy as SEH does on
// Windows, and the kernel copy is invisible to ThreadSanitizer, like the engine's racy-by-design optimistic reads);
// the lock words are real atomics (seq_cst, as Interlocked*), so TSan checks every gate ordering.
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>

#include "memory.h"

namespace kiln::engine::mem {
std::atomic<std::uintptr_t> test_image{0};
std::atomic<std::uint64_t> test_reads{0};
// Called with the 1-based number of each read before it copies: a test mutates engine memory at an exact read.
std::atomic<void (*)(std::uint64_t)> test_read_hook{nullptr};

bool read_span(std::uintptr_t address, void* out, std::size_t size) {
    if (!address || address > UINTPTR_MAX - size) return false;
    const std::uint64_t n = test_reads.fetch_add(1, std::memory_order_relaxed) + 1;
    if (const auto hook = test_read_hook.load(std::memory_order_relaxed)) hook(n);
    if (!size) return true;
    iovec local{out, size}, remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

std::atomic<std::uint64_t> test_cas{0};
bool try_shared(std::uintptr_t address) {
    if (!address || address % alignof(std::int32_t)) return false;
    test_cas.fetch_add(1, std::memory_order_relaxed);
    auto* word = reinterpret_cast<std::int32_t*>(address);
    for (int i = 0; i < 128; ++i) {  // as memory_win.cpp: retried only while the word stays Shared
        std::int32_t current = __atomic_load_n(word, __ATOMIC_SEQ_CST);
        if (current < 0 || current >= 0x7fffffff) return false;
        if (__atomic_compare_exchange_n(word, &current, current + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return true;
    }
    return false;
}

bool try_exclusive(std::uintptr_t address) {
    if (!address || address % alignof(std::int32_t)) return false;
    test_cas.fetch_add(1, std::memory_order_relaxed);
    std::int32_t expected = 0;
    return __atomic_compare_exchange_n(reinterpret_cast<std::int32_t*>(address), &expected,
                                       static_cast<std::int32_t>(0x80000000u), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

std::atomic<std::uint64_t> test_releases{0};
// Set by gate_test: says whether Kiln's holder is still taken. Only the gate takes the Scene word exclusively, so every
// exclusive release is probed. A shared release is the gate's own only inside gate_release (test_in_gate_release, set
// by gate_test through the gate's release test point); reads' registry shares outside any gate are not probed.
bool (*test_holder_probe)() = nullptr;
std::atomic<std::uint64_t> test_releases_without_holder{0}, test_gate_shared_releases{0};
thread_local bool test_in_gate_release = false;
void release_shared(std::uintptr_t address) noexcept {
    test_releases.fetch_add(1, std::memory_order_relaxed);
    if (test_holder_probe && test_in_gate_release) {
        test_gate_shared_releases.fetch_add(1, std::memory_order_relaxed);
        if (!test_holder_probe()) test_releases_without_holder.fetch_add(1, std::memory_order_relaxed);
    }
    __atomic_fetch_sub(reinterpret_cast<std::int32_t*>(address), 1, __ATOMIC_SEQ_CST);
}

void release_exclusive(std::uintptr_t address) noexcept {
    test_releases.fetch_add(1, std::memory_order_relaxed);
    if (test_holder_probe && !test_holder_probe()) test_releases_without_holder.fetch_add(1, std::memory_order_relaxed);
    __atomic_store_n(reinterpret_cast<std::int32_t*>(address), 0, __ATOMIC_SEQ_CST);
}

// Thread priorities, simulated per thread (Windows values: -1 below normal, 0 normal, 1 above normal). Each boost and
// restore records the CAS and release counts at that moment, so tests prove the order around the engine words.
thread_local int test_priority = -1;
thread_local std::uint64_t test_boost_cas = 0, test_restore_releases = 0;
thread_local int test_boosts = 0, test_restores = 0;
bool boost_priority(int* saved) {
    if (test_priority >= 1) return false;
    *saved = test_priority;
    test_priority = 1;
    test_boost_cas = test_cas.load();
    ++test_boosts;
    return true;
}
void restore_priority(int saved) noexcept {
    test_priority = saved;
    test_restore_releases = test_releases.load();
    ++test_restores;
}

// Writes go through process_vm_writev: an unmapped or read-only page fails the write as SEH does on Windows.
std::atomic<std::uint64_t> test_writes{0};
bool write_span(std::uintptr_t address, const void* data, std::size_t size) {
    if (!address || address > UINTPTR_MAX - size) return false;
    test_writes.fetch_add(1, std::memory_order_relaxed);
    if (!size) return true;
    iovec local{const_cast<void*>(data), size}, remote{reinterpret_cast<void*>(address), size};
    return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

// Recorded tracker calls; a test can make the next call "fault".
std::atomic<int> test_tracker_calls{0};
std::atomic<bool> test_tracker_fault{false};
std::atomic<std::uintptr_t> test_tracker_last_entity{0};
bool call_tracker(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t entity, std::uint16_t) {
    test_tracker_calls.fetch_add(1);
    test_tracker_last_entity.store(entity);
    return !test_tracker_fault.exchange(false);
}

std::uintptr_t image_base() { return test_image.load(std::memory_order_acquire); }
}  // namespace kiln::engine::mem
