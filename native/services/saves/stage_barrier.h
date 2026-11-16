// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
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
//
// Shutdown. cap(at) ends every wait at `at` at the latest, also a wait already running (the game's shutdown gives its
// last save 10 s in all: a world write that begins waiting after the shutdown began must not take its own full limit);
// uncap() lifts it (the next hosted world).
//
// Numbers are serial: compared by their difference, so the wrap after 2^64 stagings changes nothing (the managed side
// skips 0, which stage_progress reads as "unchanged").

// How long the Save2Thread waits for the writer before a world write (saves.cpp hk_write): no longer than the shutdown's
// wait, for every save (review 3). No shutdown signal reaches kiln.saves in time on every build: the shutdown's save
// request names its reason only with saves.reasons (and the game's shutdown skips that request in one case), the game's
// shutdown line comes through Kiln's log thread, which a slow kiln.log write can hold for seconds, and the world's stop
// comes after the last save. So a world write that began waiting before a shutdown nobody saw still goes inside the
// game's 10 s, with room for the write itself.
inline constexpr std::chrono::milliseconds kWorldWriteWait{6000};
// Once the shutdown is seen, every wait ends this long after the shutdown began at the latest (StageBarrier::cap).
inline constexpr std::chrono::milliseconds kShutdownWait{6000};
// The game's own budget for its last save: serverShutdown waits for a save in flight at most 10 s (docs/re/saves.md).
inline constexpr std::chrono::milliseconds kGameShutdownBudget{10000};

class StageBarrier {
public:
    using clock = std::chrono::steady_clock;

    struct Wait {
        bool needed = false;  // the writer was behind when the world write began
        bool off = false;     // not waited: an earlier wait ran out and the writer has not caught up since
        bool caught = false;  // the writer caught up within the limit (or nothing was needed)
        bool capped = false;  // the shutdown cap ended it before its own limit
        uint64_t target = 0, reached = 0;
        double ms = 0;
    };

    static bool later(uint64_t a, uint64_t b) { return static_cast<int64_t>(a - b) > 0; }
    static bool reached(uint64_t done, uint64_t target) { return static_cast<int64_t>(done - target) >= 0; }

    void issued(uint64_t n) {
        std::lock_guard lk(mu_);
        if (later(n, issued_)) issued_ = n;
    }

    void done(uint64_t n) {
        {
            std::lock_guard lk(mu_);
            if (later(n, done_)) done_ = n;
            if (off_ && reached(done_, off_until_)) off_ = false;
        }
        cv_.notify_all();
    }

    void cap(clock::time_point at) {
        {
            std::lock_guard lk(mu_);
            if (!capped_ || at < cap_) cap_ = at, capped_ = true;
        }
        cv_.notify_all();
    }

    void uncap() {
        std::lock_guard lk(mu_);
        capped_ = false;
    }

    Wait wait(std::chrono::milliseconds limit) {
        Wait w;
        const auto t0 = clock::now();
        const auto own = t0 + limit;
        std::unique_lock lk(mu_);
        w.target = issued_;
        w.reached = done_;
        if (reached(done_, w.target)) return w.caught = true, w;
        w.needed = true;
        if (off_) return w.off = true, w;
        while (!reached(done_, w.target)) {
            const auto until = capped_ ? std::min(own, cap_) : own;
            if (clock::now() >= until) break;
            cv_.wait_until(lk, until);
        }
        w.caught = reached(done_, w.target);
        w.reached = done_;
        w.capped = !w.caught && capped_ && cap_ < own;
        if (!w.caught) off_ = true, off_until_ = w.target;
        w.ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
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
    bool off_ = false;
    uint64_t off_until_ = 0;  // off until done reaches it
    bool capped_ = false;
    clock::time_point cap_{};
};

// The shutdown's signals and the next world's start, against one StageBarrier (saves.cpp). A signal's deadline counts from
// when the shutdown began, kShutdownWait later; for the game's shutdown line that is when the game logged it, and a line
// logged before the current world started is an earlier world's (delivered late): ignored. The check and the cap are one
// step against world_started (review 3): a line checked just before the next world started and capped just after would
// put the old world's deadline, long past, on every save of the new one. Lock order: this, then the barrier's.
class ShutdownCap {
public:
    using clock = StageBarrier::clock;
    enum class Seen { Ignored, First, Again };

    explicit ShutdownCap(StageBarrier& barrier) : barrier_(barrier) {}

    // The shutdown began at `at`. `logged`: `at` is when the game logged its shutdown line (its world is checked).
    Seen begins(clock::time_point at, bool logged) {
        std::lock_guard lk(mu_);
        if (logged && at < started_) return Seen::Ignored;
        if (between) between(between_user);
        barrier_.cap(at + kShutdownWait);
        return seen_ ? Seen::Again : (seen_ = true, Seen::First);
    }

    // The next world started at `now`: its saves wait with their own limit again; a line logged before now is the old one's.
    void world_started(clock::time_point now) {
        std::lock_guard lk(mu_);
        started_ = now;
        seen_ = false;
        barrier_.uncap();
    }

    // Tests only: runs inside begins(), after a line's check and before its cap.
    void (*between)(void*) = nullptr;
    void* between_user = nullptr;

private:
    std::mutex mu_;
    StageBarrier& barrier_;
    clock::time_point started_{};
    bool seen_ = false;
};
}  // namespace ksaves
