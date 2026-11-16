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
    return failures ? 1 : 0;
}
