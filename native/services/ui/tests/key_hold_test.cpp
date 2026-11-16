// SPDX-License-Identifier: MIT
// key_hold.h: one press counts once (GitHub #39, Nexus 179900169), whatever the focus does. Plain logic; the window
// procedure around it is tests/window_input/harness.cpp (Wine).
#include "../key_hold.h"

#include <cstdio>

namespace {
int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    ++(ok ? g_pass : g_fail);
}
using kui::hold::Prev;
constexpr Prev kRaw = Prev::unknown;
}  // namespace

int main() {
    constexpr uint32_t F8 = 0x77, J = 'J';
    check(kui::hold::window_for_delay(0) == 1500 && kui::hold::window_for_delay(1) == 1500 &&
              kui::hold::window_for_delay(3) == 1750 && kui::hold::window_for_delay(9) == 1750,
          "the window is the system's repeat delay and 750 ms, at least 1500 ms");
    {
        kui::hold::Keys k;
        check(!k.down(F8, kRaw, 1000).repeat && k.held(F8), "a first down is a press, and the key is held");
        check(k.down(F8, kRaw, 1600).repeat, "the first repeat after a 600 ms repeat delay is a repeat");
        check(k.down(F8, kRaw, 1640).repeat && k.down(F8, kRaw, 3000).repeat,
              "repeats restart the window (a long hold stays one press)");
        k.up(F8, 3050);
        check(!k.held(F8), "a release ends the hold");
        const auto d = k.down(F8, Prev::up, 3080);
        check(!d.repeat && d.since_press_ms == 2080 && d.since_release_ms == 30,
              "after a release the next down is a press, also 30 ms later (2080 ms after the last press)");
    }
    {
        kui::hold::Keys k;  // the 40 ms case: a press's raw record, a focus loss, then the same press's key message
        k.down(F8, kRaw, 100);
        check(k.down(F8, Prev::up, 140).repeat, "the same press's key message 40 ms later is no second press");
        check(k.down(F8, Prev::up, 140).repeat, "also at the same tick");
    }
    {
        kui::hold::Keys k;  // a raw repeat handled ahead of the press's first key message
        k.down(J, kRaw, 1000);
        check(k.down(J, kRaw, 1250).repeat, "a raw repeat 250 ms later is a repeat");
        check(k.down(J, Prev::up, 1251).repeat, "and the press's first key message after it is no second press");
    }
    {
        kui::hold::Keys k;  // fast taps with their releases seen
        k.down(J, kRaw, 10);
        k.up(J, 40);
        check(!k.down(J, kRaw, 60).repeat, "two taps 50 ms apart, releases seen: two presses");
    }
    {
        kui::hold::Keys k;  // the release went to another window (a focus loss): the press after the window is a press
        k.down(J, kRaw, 10);
        k.focus_back([](uint32_t) { return false; });
        check(!k.held(J), "the focus back with J up: J is not held");
        check(k.down(J, kRaw, 1700).repeat == false, "a down 1.69 s later is a press (beyond the 1.5 s window)");
        kui::hold::Keys m;
        m.down(J, kRaw, 10);
        m.focus_back([](uint32_t) { return false; });
        check(m.down(J, kRaw, 700).repeat, "the rule's known miss: a down 690 ms after a release Kiln never saw is a repeat");
    }
    {
        kui::hold::Keys k;  // the focus's return changes no press
        k.down(J, kRaw, 10);
        k.focus_back([](uint32_t vk) { return vk == 'J'; });
        check(k.held(J) && k.down(J, kRaw, 60).repeat, "held through the focus's return: its repeat is a repeat");
        kui::hold::Keys m;
        m.focus_back([](uint32_t vk) { return vk == 'K'; });
        check(!m.held('K') && !m.down('K', kRaw, 5).repeat, "a key the focus's return sees down is not made held: its down is a press");
    }
    {
        kui::hold::Keys k;  // a key message's previous-state bit (lParam bit 30)
        check(kui::hold::prev_of_lparam(0x00420001) == Prev::up && kui::hold::prev_of_lparam(0x40420001) == Prev::down &&
                  kui::hold::prev_of_lparam(static_cast<int64_t>(0xC0420001u)) == Prev::down,
              "lParam bit 30 is the previous state");
        check(k.down('L', Prev::down, 5).repeat, "a down whose message says the key was down already is a repeat");
        check(!k.held(0) && !k.held(256) && !k.down(0, Prev::up, 1).repeat && !k.down(300, Prev::up, 1).repeat,
              "codes outside 1..255 are never held");
        kui::hold::Keys m;
        m.set_window(2000);
        m.down('M', kRaw, 10);
        check(m.window() == 2000 && m.down('M', kRaw, 1900).repeat, "a longer window from the system's repeat delay");
    }
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
