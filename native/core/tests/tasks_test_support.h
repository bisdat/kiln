// SPDX-License-Identifier: MIT
// Shared helpers for the kiln.tasks host tests (test-tasks.sh): checks, a reusable start barrier for forced overlap
// windows, and command-selected test dispatch (`tasks_model_test <name>` runs one test, no argument runs all).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace kiln::tasks::test {

inline std::atomic<int> g_checks{0}, g_failures{0};

inline void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

// A one-shot gate: wait() blocks until open() (a test-only overlap window; never used by the model itself).
class Gate {
public:
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }
    void wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return open_; });
    }
    bool is_open() {
        std::lock_guard lock(mutex_);
        return open_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
};

// N threads released together, so their critical calls overlap as closely as the scheduler allows.
class StartLine {
public:
    explicit StartLine(int parties) : parties_(parties) {}
    void arrive() {
        std::unique_lock lock(mutex_);
        if (++arrived_ == parties_) {
            cv_.notify_all();
            return;
        }
        cv_.wait(lock, [&] { return arrived_ >= parties_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int parties_, arrived_ = 0;
};

using TestFn = std::function<void()>;
inline std::vector<std::pair<const char*, TestFn>>& registry() {
    static std::vector<std::pair<const char*, TestFn>> tests;
    return tests;
}
struct Register {
    Register(const char* name, TestFn fn) { registry().emplace_back(name, std::move(fn)); }
};

// Runs the named test (or every test), prints one summary line, returns the process exit code.
inline int run(int argc, char** argv, const char* suite) {
    int ran = 0;
    for (auto& [name, fn] : registry()) {
        if (argc > 1 && std::strcmp(argv[1], name) != 0) continue;
        ++ran;
        fn();
    }
    if (argc > 1 && !ran) {
        std::fprintf(stderr, "no test named %s\n", argv[1]);
        return 2;
    }
    std::printf("%s: %d tests, %d checks, %d failures\n", suite, ran, g_checks.load(), g_failures.load());
    return g_failures.load() ? 1 : 0;
}

}  // namespace kiln::tasks::test

#define KILN_TASKS_TEST(name)                                                       \
    static void name();                                                             \
    static const ::kiln::tasks::test::Register register_##name(#name, &name);       \
    static void name()
