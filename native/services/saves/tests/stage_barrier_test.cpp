// SPDX-License-Identifier: MIT
// kiln.saves' stage barrier (stage_barrier.h): the game's world write waits for Kiln.Saves' writer, bounded.
#include "../stage_barrier.h"
#include <atomic>
#include <cstdio>
#include <thread>

int main() {
    using namespace std::chrono_literals;
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; };

    ksaves::StageBarrier b;
    auto w = b.wait(1000ms);
    check(w.caught && !w.needed && w.ms == 0, "nothing issued: the world write does not wait");

    b.issued(1);
    b.done(1);
    w = b.wait(1000ms);
    check(w.caught && !w.needed, "the writer already has the staging on disk: no wait");

    b.issued(3);  // the save request's staging and the snapshot staging of the next save
    std::atomic<bool> released{false};
    std::thread writer([&] {
        std::this_thread::sleep_for(150ms);
        b.done(2);
        std::this_thread::sleep_for(50ms);
        released = true;
        b.done(3);
    });
    w = b.wait(5000ms);
    writer.join();
    check(w.needed && w.caught && released && w.target == 3 && w.reached == 3 && w.ms >= 150,
          "the world write waits until the writer has every staging issued before it (not only the first)");

    b.issued(4);
    w = b.wait(100ms);
    check(w.needed && !w.caught && !w.off && w.target == 4 && w.reached == 3 && w.ms >= 95,
          "a writer that does not catch up within the limit lets the world write go");
    b.issued(5);
    const auto t0 = std::chrono::steady_clock::now();
    w = b.wait(5000ms);
    check(w.needed && w.off && !w.caught && std::chrono::steady_clock::now() - t0 < 50ms,
          "after a wait ran out, the next world write does not wait for a writer that has not caught up (one bounded wait, not one per save)");
    b.done(4);
    std::thread late([&] {
        std::this_thread::sleep_for(100ms);
        b.done(5);
    });
    w = b.wait(5000ms);
    late.join();
    check(w.needed && w.caught && !w.off && w.reached == 5,
          "once the writer caught up with what was issued when the wait ran out, the barrier holds again");

    b.issued(6);
    b.issued(5);  // numbers only grow
    b.done(6);
    b.done(2);
    w = b.wait(1000ms);
    check(w.caught && !w.needed && b.issued_value() == 6 && b.done_value() == 6, "issued and done never go back");

    // Shutdown (review 1): the game's shutdown gives its last save 10 s in all, whatever the save's reason says.
    {
        ksaves::StageBarrier s;
        s.issued(1);
        const auto t1 = std::chrono::steady_clock::now();
        std::thread stop([&] {
            std::this_thread::sleep_for(100ms);
            s.cap(std::chrono::steady_clock::now() + 100ms);  // the shutdown begins while an autosave's world write waits
        });
        auto c = s.wait(30000ms);
        stop.join();
        const auto took = std::chrono::steady_clock::now() - t1;
        check(c.needed && !c.caught && c.capped && took >= 190ms && took < 2000ms,
              "a wait already running with 30 s is cut short when the shutdown begins");
        s.done(1);
        s.issued(2);
        const auto t2 = std::chrono::steady_clock::now();
        s.cap(std::chrono::steady_clock::now() + 100ms);
        c = s.wait(30000ms);
        check(c.needed && !c.caught && c.capped && std::chrono::steady_clock::now() - t2 < 2000ms,
              "a world write after the shutdown began waits until the shutdown's deadline at the latest");
        s.done(2);
        s.uncap();
        s.issued(3);
        std::thread w3([&] {
            std::this_thread::sleep_for(300ms);
            s.done(3);
        });
        c = s.wait(5000ms);
        w3.join();
        check(c.caught && !c.capped && c.ms >= 290, "the next world's saves wait with their own limit again (uncap)");
    }

    // A shutdown line delivered late (review 2): its deadline counts from when the game logged it; one already past
    // applies at once.
    {
        ksaves::StageBarrier p;
        p.issued(1);
        p.cap(std::chrono::steady_clock::now() - 1000ms);
        const auto t0 = std::chrono::steady_clock::now();
        const auto c = p.wait(30000ms);
        check(c.needed && !c.caught && c.capped && std::chrono::steady_clock::now() - t0 < 100ms,
              "a shutdown deadline already past ends the wait at once");
    }

    // A shutdown nobody tells kiln.saves about in time (review 3): saves.reasons is off, so the shutdown's save request
    // says nothing, and the game's shutdown line waits behind a slow kiln.log write on Kiln's log thread. The world write
    // of an autosave already waiting must still go inside the game's 10 s for its last save, with the limit saves.cpp
    // uses (kWorldWriteWait), not only once the line arrives.
    {
        ksaves::StageBarrier q;
        q.issued(1);  // the writer never catches up (a disk that does not answer)
        std::atomic<bool> waiting{false};
        std::chrono::steady_clock::time_point ended{};
        std::thread save2([&] {
            waiting = true;
            q.wait(ksaves::kWorldWriteWait);
            ended = std::chrono::steady_clock::now();
        });
        while (!waiting) std::this_thread::yield();
        std::this_thread::sleep_for(200ms);
        const auto shutdown = std::chrono::steady_clock::now();  // the game logs its line; the callback is held back
        save2.join();                                             // ... until the world write went
        q.cap(shutdown + ksaves::kShutdownWait);                  // the line, delivered late: its deadline from when it was logged
        const auto after = std::chrono::duration_cast<std::chrono::milliseconds>(ended - shutdown).count();
        std::printf("     the world write went %lld ms after the shutdown began (the game gives %lld ms)\n", static_cast<long long>(after),
                    static_cast<long long>(ksaves::kGameShutdownBudget.count()));
        check(ended - shutdown <= ksaves::kShutdownWait + 500ms,
              "with the shutdown's signal held back while a world write waits, the write still goes within the shutdown's wait");
        check(ksaves::kWorldWriteWait <= ksaves::kShutdownWait && ksaves::kShutdownWait < ksaves::kGameShutdownBudget,
              "no world write waits longer than the shutdown's wait, and that leaves the world write room in the game's 10 s");
    }

    // Numbers wrap after 2^64 stagings (the managed side skips 0): still ordered. Serial numbers only move forward by less
    // than 2^63, so the counters walk there in quarter steps (review 2: a jump from 0 to near the end reads as backwards).
    {
        ksaves::StageBarrier r;
        const uint64_t max = UINT64_MAX;
        for (const uint64_t n : {uint64_t{1} << 62, uint64_t{2} << 62, uint64_t{3} << 62, max - 1}) {
            r.issued(n);
            r.done(n);
        }
        check(r.issued_value() == max - 1 && r.done_value() == max - 1, "the counters stand just before the wrap");
        r.issued(max);
        r.issued(1);  // the next after max (0 is skipped)
        check(r.issued_value() == 1, "the staging after the wrap is the newest");
        auto c = r.wait(50ms);
        check(c.needed && !c.caught && c.target == 1 && c.reached == max - 1, "a staging issued after the wrap is waited for");
        r.done(max);
        check(r.done_value() == max, "the writer's staging before the wrap counts");
        r.done(1);
        c = r.wait(1000ms);
        check(c.caught && !c.needed && r.done_value() == 1, "it counts as done once the writer reports it, across the wrap");
        r.done(max);
        check(r.done_value() == 1, "an older number after the wrap does not move done back");
    }
    return failures ? 1 : 0;
}
