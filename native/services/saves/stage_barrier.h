// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace ksaves {
// Kiln.Saves writes a save's files on a background writer, off the world's tick (since 0.2.0-dev.29.3, issue #39). The tick
// thread used to write a generation's pending manifest before the game's snapshot, so a world file could never reach the
// disk before the manifest that names its mod data. This keeps that order where it costs the tick nothing: the pump
// reports each staging it hands its writer (issued, numbers grow), the writer reports the newest one it has on disk
// (done), and before the game's Save2Thread writes a world file it waits until done reaches what was issued by then.
//
// Bounded. A wait that runs out lets the world write go (a crash before the writer finishes then loads the older
// generation, which the caller logs) and switches the barrier off until the writer catches up with what was issued
// when it ran out: a writer that hangs costs one bounded wait, not one per save; one that is only slow is waited for
// again as soon as it has caught up. done() says when that catch-up happens (since 0.2.0-dev.31.5; WaitNotes below decides
// what kiln.log says about both).
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

    // What done() reports: the writer reached what was issued when a wait ran out (the barrier holds again).
    struct Caught {
        bool caught_up = false;
        uint64_t target = 0;          // the staging that wait was for
        clock::time_point went_ahead{};  // when it ran out: its world write went ahead without that staging
    };

    static bool later(uint64_t a, uint64_t b) { return static_cast<int64_t>(a - b) > 0; }
    static bool reached(uint64_t done, uint64_t target) { return static_cast<int64_t>(done - target) >= 0; }

    void issued(uint64_t n) {
        std::lock_guard lk(mu_);
        if (later(n, issued_)) issued_ = n;
    }

    Caught done(uint64_t n) {
        Caught c;
        {
            std::lock_guard lk(mu_);
            if (later(n, done_)) done_ = n;
            if (off_ && reached(done_, off_until_)) {
                off_ = false;
                c = {true, off_until_, off_at_};
            }
        }
        cv_.notify_all();
        return c;
    }

    // A wait ran out and the writer has not caught up since (the next world write does not wait).
    bool behind() {
        std::lock_guard lk(mu_);
        return off_;
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
        const auto end = clock::now();
        if (!w.caught) off_ = true, off_until_ = w.target, off_at_ = end;
        w.ms = std::chrono::duration<double, std::milli>(end - t0).count();
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
    clock::time_point off_at_{};  // when the wait that switched it off ran out
    bool capped_ = false;
    clock::time_point cap_{};
};

// What kiln.log says about world writes that go ahead of Kiln.Saves' writer (saves.cpp; since 0.2.0-dev.31.5). A writer
// that is only slow is a delay, not an error (on one player's PC every snapshot staging's renames took 6-25 s for hours,
// so nearly every autosave's world write went ahead after its 6 s, and kiln.log had a warning per save): the first world
// write of a world session that goes ahead is said in full, and so is the writer's first catch-up after a wait ran out;
// later ones are counted and summed up at most every kSummaryEvery after the last line (how many went ahead, after a wait
// or without one, the longest wait, how often and how late the writer caught up), at the first world write or catch-up
// after that, and when the world stops. A staging that cannot be written is Kiln.Saves' error to log (managed), not this.
// A wait cut short by the shutdown is not counted here: saves.cpp says each one. Any thread (the Save2Thread, the writer
// through stage_progress, the tick thread): one lock.
inline constexpr std::chrono::seconds kSummaryEvery{300};

class WaitNotes {
public:
    using clock = StageBarrier::clock;
    enum class Say { Nothing, Full };  // Full: the first of its kind in this world session, logged in full

    struct Summary {
        uint32_t waited = 0;    // world writes that went ahead after their wait ran out
        uint32_t unwaited = 0;  // ... without waiting: the writer was still behind since an earlier one
        double longest_wait_ms = 0;
        uint32_t caught = 0;    // the writer's catch-ups
        double slowest_catch_up_ms = 0;  // from a world write going ahead to the writer having its staging on disk
        double minutes = 0;     // since the last line
    };

    // A world write went ahead of the writer (`w`: its wait ran out, not for the shutdown, or it did not wait).
    Say went_ahead(const StageBarrier::Wait& w, clock::time_point now) {
        std::lock_guard lk(mu_);
        if (!ahead_said_) {
            ahead_said_ = true;
            last_ = now;
            return Say::Full;
        }
        if (w.off) {
            ++s_.unwaited;
        } else {
            ++s_.waited;
            s_.longest_wait_ms = std::max(s_.longest_wait_ms, w.ms);
        }
        return Say::Nothing;
    }

    // The writer caught up `ms` after a world write went ahead without its staging (StageBarrier::done).
    Say caught_up(double ms, clock::time_point now) {
        std::lock_guard lk(mu_);
        if (!caught_said_) {
            caught_said_ = true;
            last_ = now;
            return Say::Full;
        }
        ++s_.caught;
        s_.slowest_catch_up_ms = std::max(s_.slowest_catch_up_ms, ms);
        return Say::Nothing;
    }

    // What was counted since the last line, when kSummaryEvery has passed since it (`force`: now, the world stops);
    // counting starts from zero again.
    std::optional<Summary> summary(clock::time_point now, bool force = false) {
        std::lock_guard lk(mu_);
        if (s_.waited + s_.unwaited + s_.caught == 0 || (!force && now - last_ < kSummaryEvery)) return std::nullopt;
        Summary out = s_;
        out.minutes = std::chrono::duration<double, std::ratio<60>>(now - last_).count();
        s_ = {};
        last_ = now;
        return out;
    }

    // The next world session: its first ones are said in full again.
    void world_started() {
        std::lock_guard lk(mu_);
        ahead_said_ = caught_said_ = false;
        s_ = {};
    }

private:
    std::mutex mu_;
    bool ahead_said_ = false, caught_said_ = false;
    Summary s_{};
    clock::time_point last_{};
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
