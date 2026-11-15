// SPDX-License-Identifier: MIT
// The facts a stop writes boot.json from (boot.cpp: kiln.boot stop_start's state writer and the late hold of an
// activation fault). A copy owned for the process's life: boot() replaces it under the lock after each change of its
// own facts, and a writer reads and extends that copy under the same lock. No writer ever touches boot()'s frame, so a
// stop from any thread, at any moment (boot() still running, copying or returned), sees whole facts; a stop on the boot
// thread itself cannot deadlock, since boot() holds the lock only while it copies. Host test: tests/stop_facts_test.cpp.
#pragma once
#include <mutex>

namespace kiln {

template <class Facts>
class FactsBox {
public:
    void publish(const Facts& f) {
        std::lock_guard lk(mu_);
        facts_ = f;
        has_ = true;
    }
    // fn(Facts&) on the box's own copy, under its lock. False: nothing was published yet (fn not called).
    template <class Fn>
    bool with(Fn&& fn) {
        std::lock_guard lk(mu_);
        if (!has_) return false;
        fn(facts_);
        return true;
    }

private:
    std::mutex mu_;
    Facts facts_{};
    bool has_ = false;
};

}  // namespace kiln
