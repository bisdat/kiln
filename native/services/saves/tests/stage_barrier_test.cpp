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

    // The shutdown's line and the next world's start (ShutdownCap, saves.cpp's on_game_log and on_world).
    {
        using clock = std::chrono::steady_clock;
        ksaves::StageBarrier g;
        ksaves::ShutdownCap sc(g);
        const auto old_start = clock::now() - 30s;
        sc.world_started(old_start);
        check(sc.begins(old_start - 1s, true) == ksaves::ShutdownCap::Seen::Ignored,
              "a shutdown line logged before the world started is an earlier world's: ignored");
        // Review 3: the old world's line passes its check (logged after the old world started), its callback pauses there,
        // the next world starts and lifts the cap, then the callback goes on. It must not put the old world's deadline,
        // long past, on the new world's saves (each would go at once, and the barrier would switch itself off).
        struct Ctx {
            ksaves::ShutdownCap* sc;
            std::thread next;
        } ctx{&sc, {}};
        sc.between_user = &ctx;
        sc.between = [](void* u) {
            auto* c = static_cast<Ctx*>(u);
            c->next = std::thread([c] { c->sc->world_started(std::chrono::steady_clock::now()); });  // the next world starts
            std::this_thread::sleep_for(std::chrono::milliseconds(200));                              // ... meanwhile
        };
        sc.begins(old_start + 5s, true);
        sc.between = nullptr;
        ctx.next.join();
        g.issued(1);
        std::thread wr([&] {
            std::this_thread::sleep_for(200ms);
            g.done(1);
        });
        const auto c = g.wait(ksaves::kWorldWriteWait);
        wr.join();
        check(c.caught && !c.capped && c.ms >= 190,
              "an earlier world's shutdown line checked just before the next world started does not cap the next world's saves");
        check(sc.begins(clock::now(), true) == ksaves::ShutdownCap::Seen::First,
              "the next world's own shutdown is seen (and logged) as its first");
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

    // A writer that is only slow (0.2.0-dev.31.5: on one player's PC every snapshot staging's renames took 6-25 s, so
    // nearly every autosave's world write went ahead after its 6 s): the barrier reports the writer's catch-up after a wait
    // ran out, once, with the staging and when that world write went ahead.
    {
        ksaves::StageBarrier s;
        s.issued(1);
        const auto before = std::chrono::steady_clock::now();
        const auto ran = s.wait(20ms);
        check(!ran.caught && s.behind(), "a wait that ran out leaves the barrier behind");
        auto c = s.done(1);
        check(c.caught_up && c.target == 1 && c.went_ahead >= before && c.went_ahead <= std::chrono::steady_clock::now() &&
                  !s.behind(),
              "the writer's catch-up is reported with its staging and when the world write went ahead");
        c = s.done(1);
        check(!c.caught_up, "a catch-up is reported once");
        s.issued(2);
        c = s.done(2);
        check(!c.caught_up && !s.behind(), "a staging on disk before any wait ran out is no catch-up");
        s.issued(3);
        s.wait(10ms);
        s.issued(4);
        c = s.done(3);
        check(c.caught_up && c.target == 3, "the catch-up is the staging the world write that went ahead waited for");
    }

    // What kiln.log says about it (WaitNotes, saves.cpp): a delay, not an error. The first world write ahead of the writer
    // in a world session is said in full, and so is the writer's first catch-up; later ones are counted and summed up at
    // most every 5 minutes; the world's stop says what is counted at once; the next world session starts over.
    {
        using clock = std::chrono::steady_clock;
        using Say = ksaves::WaitNotes::Say;
        ksaves::WaitNotes n;
        const auto t0 = clock::now();
        ksaves::StageBarrier::Wait ran{}, off{};
        ran.needed = true;
        ran.ms = 6000;
        off.needed = true;
        off.off = true;
        check(n.went_ahead(ran, t0) == Say::Full, "the first world write ahead of the writer in a world session is said in full");
        check(n.caught_up(14000, t0 + 14s) == Say::Full, "and so is the writer's first catch-up");
        check(!n.summary(t0 + 15s), "nothing counted yet: no summary");
        check(n.went_ahead(ran, t0 + 30s) == Say::Nothing && n.went_ahead(off, t0 + 60s) == Say::Nothing &&
                  n.caught_up(20004.6, t0 + 50s) == Say::Nothing && n.caught_up(9000, t0 + 80s) == Say::Nothing,
              "later ones are counted, not logged one by one");
        check(!n.summary(t0 + 14s + 299s), "a summary at most every 5 minutes after the last line");
        const auto s = n.summary(t0 + 14s + 300s);
        check(s && s->waited == 1 && s->unwaited == 1 && s->caught == 2 && s->longest_wait_ms == 6000 &&
                  s->slowest_catch_up_ms == 20004.6 && s->minutes > 4.9 && s->minutes < 5.1,
              "the summary says how many went ahead (after a wait, without one), the longest wait and the slowest catch-up");
        check(!n.summary(t0 + 14s + 301s) && !n.summary(t0 + 1h), "it counts from zero again: nothing new, no summary");
        n.went_ahead(ran, t0 + 2h);
        const auto again = n.summary(t0 + 2h + 1s);
        check(again && again->waited == 1 && again->minutes > 100, "a delay after a quiet hour is summed up at once");
        n.went_ahead(off, t0 + 2h + 30s);
        check(!n.summary(t0 + 2h + 31s), "the next one waits for its 5 minutes");
        const auto stop = n.summary(t0 + 2h + 31s, true);
        check(stop && stop->unwaited == 1 && stop->waited == 0 && stop->caught == 0, "the world's stop says what is counted at once");
        n.world_started();
        check(n.went_ahead(ran, t0 + 3h) == Say::Full && n.caught_up(1000, t0 + 3h) == Say::Full,
              "the next world session says its first ones in full again");
    }
    return failures ? 1 : 0;
}
