// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
namespace kiln::decisions {
struct LockUnavailable {};
// Set once before hooks are published. Never calls game code or acquires a lock.
inline std::atomic<bool (*)()> lock_stopped{nullptr};
inline std::atomic<uint64_t> lock_bypasses{0};
class BoundedMutex {
public:
    void lock() {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        for (unsigned n = 0; n < 4096; ++n) {
            if (m_.try_lock()) return;
            const auto stopped = lock_stopped.load(std::memory_order_acquire);
            if (stopped && stopped()) break;
            if (std::chrono::steady_clock::now() >= end) break;
            std::this_thread::yield();
        }
        lock_bypasses.fetch_add(1, std::memory_order_relaxed);
        throw LockUnavailable{};
    }
    bool try_lock() { return m_.try_lock(); }
    void unlock() { m_.unlock(); }
private:
    std::mutex m_;
};
}
