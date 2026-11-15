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
        check(!begin(finish(first.record), "sl").off); // normal exit before frame
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
