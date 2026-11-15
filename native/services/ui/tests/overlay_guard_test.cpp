#include "../overlay_guard.h"
#include <cassert>
#include <cstdio>
using namespace kui::overlay_guard;
int main() {
    unsigned checks = 0;
    auto check = [&](bool b) { ++checks; assert(b); };
    check(!begin({}, "none").off);
    for (unsigned s = 0; s <= unsigned(Stage::swapchain); ++s) {
        auto first = begin({}, "sl");
        first.record.stage = Stage(s);
        auto off = begin(first.record, "sl");
        check(off.off && off.record.failures == 1);
        check(begin(off.record, "sl").off); // killed guarded start must not earn a retry
        auto retry = begin(finish(off.record), "sl");
        check(!retry.off);
        retry.record.stage = Stage(s);
        auto locked = begin(retry.record, "sl");
        check(locked.off && locked.record.phase == Phase::locked);
        check(begin(finish(locked.record), "sl").off);
        check(!begin(locked.record, "none").off);
        check(!begin({}, "sl").off); // deleting record re-enables
        check(!begin(finish(first.record), "sl").off); // successful first drawn frame
    }
    // A start that never drew (quit from the main menu without opening Kiln's menu or a mod's window) is not a death at
    // the overlay's start once idle() counted it fine: the owner's dev.29 test, 2026-11-15.
    {
        auto start = begin({}, "m");
        start.record.stage = Stage::swapchain;
        check(!begin(start.record, "m").off == false); // without idle(): the old false alarm (guarded next start)
        auto fine = idle(start.record);
        check(fine.phase == Phase::complete && fine.failures == 0);
        check(!begin(fine, "m").off);
        // the first draw arms it again: a death before that frame is presented guards the next start
        auto armed = rearm(fine);
        check(armed.phase == Phase::trying && armed.stage == Stage::swapchain);
        auto died = begin(armed, "m");
        check(died.off && died.record.phase == Phase::guarded && died.record.failed_stage == Stage::swapchain);
        // the frame presented after the re-arm completes it as before
        auto drawn = armed; drawn.stage = Stage::frame;
        check(!begin(finish(drawn), "m").off);
        // a second death at the first draw after a retry still locks
        auto retry = begin(finish(died.record), "m").record;
        check(retry.phase == Phase::trying);
        retry.stage = Stage::swapchain;
        check(begin(rearm(idle(retry)), "m").record.phase == Phase::locked);
    }
    // the owner's record after the false alarm (retry, one failure at the swapchain): a start that idles, or exits
    // normally (idle() too), is fine
    {
        Record owner{Phase::retry, Stage::swapchain, Stage::swapchain, 1, "mb"};
        auto next = begin(owner, "mb");
        check(!next.off && next.record.phase == Phase::trying);
        next.record.stage = Stage::swapchain;
        auto fine = idle(next.record);
        check(fine.phase == Phase::complete && fine.failures == 1); // the history stays until a frame is drawn
        auto drawn = rearm(fine); drawn.stage = Stage::frame;
        check(finish(drawn).failures == 0);                         // a drawn frame clears it
        auto again = begin(fine, "mb").record;
        again.stage = Stage::swapchain;
        check(!begin(idle(again), "mb").off); // a second quiet start does not lock
        check(begin(again, "mb").off);        // a real death while trying still guards
    }
    // idle() and rearm() change nothing outside their phases
    for (Phase p : {Phase::complete, Phase::guarded, Phase::retry, Phase::locked}) {
        Record x{p, Stage::swapchain, Stage::device, 1, "m"};
        check(idle(x).phase == p && idle(x).failures == 1);
    }
    for (Phase p : {Phase::trying, Phase::guarded, Phase::retry, Phase::locked}) {
        Record x{p, Stage::device, Stage::device, 1, "m"};
        check(rearm(x).phase == p && rearm(x).stage == Stage::device);
    }
    Record r;
    check(!parse("unreadable", r));
    check(!begin(r, "sl").off);
    auto a = begin({}, "sl").record;
    check(parse(encode(a), r) && r.phase == Phase::trying && r.modules == "sl");
    check(!parse("version=99\n", r));
    check(!begin(finish(a), "sl").off);
    std::printf("overlay_guard: %u checks passed\n", checks);
}
