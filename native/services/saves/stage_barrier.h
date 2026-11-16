// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace ksaves {
// Kiln.Saves writes a save's files on a background writer, off the world's tick (since 0.2.0-dev.30, issue #39). The tick
// thread used to write a generation's pending manifest before the game's snapshot, so a world file could never reach the
// disk before the manifest that names its mod data. This keeps that order where it costs the tick nothing: the pump
// reports each staging it hands its writer (issued, numbers grow), the writer reports the newest one it has on disk
// (done), and before the game's Save2Thread writes a world file it waits until done reaches what was issued by then.
//
// Bounded. A wait that runs out lets the world write go (a crash before the writer finishes then loads the older
// generation, which the caller logs) and switches the barrier off until the writer catches up with what was issued
// when it ran out: a writer that hangs costs one bounded wait, not one per save; one that is only slow is waited for
// again as soon as it has caught up.
class StageBarrier {
public:
    struct Wait {
        bool needed = false;  // the writer was behind when the world write began
        bool off = false;     // not waited: an earlier wait ran out and the writer has not caught up since
        bool caught = false;  // the writer caught up within the limit (or nothing was needed)
        uint64_t target = 0, reached = 0;
        double ms = 0;
    };

    void issued(uint64_t n) {
        std::lock_guard lk(mu_);
        if (n > issued_) issued_ = n;
    }

    void done(uint64_t n) {
        {
            std::lock_guard lk(mu_);
            if (n > done_) done_ = n;
            if (off_until_ && done_ >= off_until_) off_until_ = 0;
        }
        cv_.notify_all();
    }

    Wait wait(std::chrono::milliseconds limit) {
        Wait w;
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_lock lk(mu_);
        w.target = issued_;
        w.reached = done_;
        if (done_ >= w.target) return w.caught = true, w;
        w.needed = true;
        if (off_until_) return w.off = true, w;
        w.caught = cv_.wait_for(lk, limit, [&] { return done_ >= w.target; });
        w.reached = done_;
        if (!w.caught) off_until_ = w.target;
        w.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return w;
    }

    uint64_t issued_value() {
        std::lock_guard lk(mu_);
        return issued_;
    }
    uint64_t done_value() {
        std::lock_guard lk(mu_);
        return done_;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    uint64_t issued_ = 0, done_ = 0;
    uint64_t off_until_ = 0;  // != 0: off until done reaches it
};
}  // namespace ksaves
