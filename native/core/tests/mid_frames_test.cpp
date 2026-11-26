// SPDX-License-Identifier: MIT
// Host test of kiln::MidFrames (../abi/kiln_mid_frames.hpp): two mid hooks of one call paired by the function's rsp.
// tests/test-mid-frames.sh builds and runs it with ASan/UBSan. No Wine, no game: the "calls" are made-up rsp values.
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "kiln_mid_frames.hpp"

static int failures = 0, checks = 0;
#define EXPECT(cond)                                                    \
    do {                                                                \
        ++checks;                                                       \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static std::vector<std::string> events;
static int alive = 0;
// A scope like the services keep in it: neither moves nor copies, says when it goes.
struct Scope {
    explicit Scope(int id, bool fail = false) : id(id) {
        if (fail) throw std::runtime_error("no");
        ++alive;
        events.push_back("open " + std::to_string(id));
    }
    ~Scope() {
        --alive;
        events.push_back("close " + std::to_string(id));
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    int id;
};

int main() {
    kiln::MidFrames<Scope, 3> f;
    // one call: the first site's scope reaches the second site of the same rsp
    Scope* a = f.enter(0x1000, 1);
    EXPECT(a && a->id == 1 && f.depth() == 1);
    EXPECT(f.leave(0x1000) == a);
    f.pop();
    EXPECT(f.depth() == 0 && alive == 0);

    // nested calls (a lower rsp is inside): inner leaves first, each finds its own
    Scope* outer = f.enter(0x2000, 2);
    Scope* inner = f.enter(0x1800, 3);
    EXPECT(outer && inner && f.depth() == 2);
    EXPECT(f.leave(0x1800) == inner);
    f.pop();
    EXPECT(f.leave(0x2000) == outer);
    f.pop();
    EXPECT(f.depth() == 0 && alive == 0);

    // a second site with no first (the hook went in while the call was inside): nothing, nothing dropped
    f.enter(0x3000, 4);
    EXPECT(f.leave(0x2800) == nullptr && f.depth() == 1);  // an inner call that never had its first site
    EXPECT(f.leave(0x3800) == nullptr && f.depth() == 0 && alive == 0);  // an outer one: the inner open call is dropped

    // a call that never reached its second site: the next call that cannot be inside it drops it
    f.enter(0x4000, 5);
    events.clear();
    Scope* next = f.enter(0x4000, 6);  // the same rsp again: the earlier call left another way
    EXPECT(next && next->id == 6 && f.depth() == 1 && alive == 1);
    EXPECT(events.size() == 2 && events[0] == "close 5" && events[1] == "open 6");
    f.enter(0x5000, 7);  // a higher rsp: it encloses nothing open, so 6 is dropped
    EXPECT(f.depth() == 1 && alive == 1);
    EXPECT(f.leave(0x5000) && f.leave(0x5000)->id == 7);
    f.pop();

    // an inner call that never left is dropped when its outer call leaves
    f.enter(0x6000, 8);
    f.enter(0x5f00, 9);
    Scope* found = f.leave(0x6000);
    EXPECT(found && found->id == 8 && f.depth() == 1 && alive == 1);
    f.pop();

    // at most N open: the next one stays the game's, the open ones are kept
    f.enter(0x9000, 10);
    f.enter(0x8000, 11);
    f.enter(0x7000, 12);
    EXPECT(f.enter(0x6000, 13) == nullptr && f.depth() == 3 && alive == 3);
    EXPECT(f.leave(0x6000) == nullptr && f.depth() == 3);  // its second site finds nothing and drops nothing
    EXPECT(f.leave(0x7000) && f.leave(0x7000)->id == 12);
    f.pop();
    f.pop();
    f.pop();
    EXPECT(f.depth() == 0 && alive == 0);
    f.pop();  // an extra pop is harmless
    EXPECT(f.depth() == 0);

    // a constructor that throws keeps nothing; rsp 0 is never a call
    EXPECT(f.enter(0xa000, 14, true) == nullptr && f.depth() == 0 && alive == 0);
    EXPECT(f.enter(0, 15) == nullptr && f.depth() == 0 && alive == 0);
    EXPECT(f.leave(0) == nullptr);

    std::printf("mid frames: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
