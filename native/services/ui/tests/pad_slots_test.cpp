// kiln#123: the pad state Kiln's screens follow (pad_slots.h). Built and run by tests/run.sh (g++, sanitizers).
#include "../pad_slots.h"

#include <cstdio>
#include <cstdlib>

using kiln::ui::PadSlots;
using kiln::ui::PadState;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static PadState pad(int16_t lx, int16_t ly, uint16_t buttons = 0) {
    PadState s{};
    s.lx = lx;
    s.ly = ly;
    s.buttons = buttons;
    return s;
}

int main() {
    // a stick pushed up and let go: the centre is recorded, so nothing goes on scrolling
    {
        PadSlots p;
        p.update(0, pad(0, 30000));
        CHECK(p.followed().ly == 30000);
        p.update(0, pad(0, 0));
        CHECK(p.followed().ly == 0);
    }
    // a stick that rests a little off centre (inside the dead zone) reads as centred
    {
        PadSlots p;
        p.update(0, pad(0, 30000));
        p.update(0, pad(-1200, 6500));
        CHECK(p.followed().lx == 0 && p.followed().ly == 0);
        p.update(0, pad(0, 7849));  // the dead zone's edge counts as outside
        CHECK(p.followed().ly == 7849);
    }
    // triggers under XInput's threshold read as 0
    {
        PadSlots p;
        PadState s{};
        s.lt = 29;
        s.rt = 30;
        p.update(0, s);
        CHECK(p.followed().lt == 0 && p.followed().rt == 30);
    }
    // an idle second (or virtual) pad does not overwrite the first one's held state
    {
        PadSlots p;
        p.update(0, pad(0, 0, 0x1000));  // A held on pad 0
        p.update(1, pad(0, 0));          // pad 1 idle, every poll
        p.update(1, pad(0, 0));
        CHECK(p.active() == 0);
        CHECK(p.followed().buttons == 0x1000);
        // pad 0 lets go: followed, and idle
        p.update(0, pad(0, 0));
        p.update(1, pad(0, 0));
        CHECK(p.active() == 0 && p.followed().buttons == 0);
    }
    // the pad that moves last is the one followed
    {
        PadSlots p;
        p.update(0, pad(0, 0));
        p.update(2, pad(-20000, 0));
        CHECK(p.active() == 2 && p.followed().lx == -20000);
        p.update(0, pad(0, 0, 0x0001));  // pad 0's d-pad up
        CHECK(p.active() == 0 && p.followed().buttons == 0x0001);
    }
    // nothing before the first poll; users past 3 are ignored
    {
        PadSlots p;
        CHECK(!p.seen());
        p.update(4, pad(0, 30000));
        CHECK(!p.seen());
        p.update(3, pad(0, 0));
        CHECK(p.seen() && p.active() == 3);
    }
    if (failures) return EXIT_FAILURE;
    std::puts("pad_slots_test: ok");
    return EXIT_SUCCESS;
}
