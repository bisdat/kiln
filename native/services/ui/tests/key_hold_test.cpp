// SPDX-License-Identifier: MIT
// key_hold.h: one press counts once, also across a focus loss (GitHub #39, Nexus 179900169). Plain logic; the window
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
}  // namespace

int main() {
    constexpr uint32_t F8 = 0x77, J = 'J';
    {
        kui::hold::Keys k;
        check(!k.down(F8, Prev::unknown, 1000).repeat, "a first down is a press");
        check(k.held(F8), "and the key is held");
        check(k.down(F8, Prev::unknown, 1500).repeat, "a down while held is a repeat (a raw record says nothing more)");
        k.up(F8, 1600);
        check(!k.held(F8), "a release ends the hold");
        const auto d = k.down(F8, Prev::up, 1700);
        check(!d.repeat && d.since_press_ms == 700 && d.since_release_ms == 100,
              "the next down is a press, 700 ms after the last press and 100 ms after the release");
    }
    {
        kui::hold::Keys k;  // the 40 ms case: the press's raw record, a focus loss, then the same press's key message
        k.down(F8, Prev::unknown, 10);
        k.focus_lost();
        check(k.held(F8) && k.away(), "a focus loss keeps the held key held");
        check(k.down(F8, Prev::up, 50).repeat, "the same press's key message after the loss is no second press");
    }
    {
        kui::hold::Keys k;  // held all through a focus loss and back
        k.down(J, Prev::unknown, 10);
        k.focus_lost();
        check(k.focus_back([](uint32_t vk) { return vk == 'J'; }) == 0 && !k.away(), "focus back with J still down: nothing changes");
        check(k.down(J, Prev::unknown, 800).repeat, "J's next raw record is a repeat");
    }
    {
        kui::hold::Keys k;  // let go while the game had no focus
        k.down(J, Prev::unknown, 10);
        k.focus_lost();
        check(k.focus_back([](uint32_t) { return false; }) == 1, "focus back with J up: J is no longer held");
        const auto d = k.down(J, Prev::unknown, 900);
        check(!d.repeat && d.since_release_ms == kui::hold::kNone && d.since_press_ms == 890,
              "J's next down is a press, with no release seen since the last one");
    }
    {
        kui::hold::Keys k;  // pressed while away and still down: its downs are repeats (the press was another window's)
        k.focus_lost();
        k.focus_back([](uint32_t vk) { return vk == 'K'; });
        check(k.held('K') && k.down('K', Prev::unknown, 5).repeat, "a key went down while away and is held: its downs are repeats");
    }
    {
        kui::hold::Keys k;  // a key message's previous-state bit (lParam bit 30)
        check(kui::hold::prev_of_lparam(0x00420001) == Prev::up && kui::hold::prev_of_lparam(0x40420001) == Prev::down &&
                  kui::hold::prev_of_lparam(static_cast<int64_t>(0xC0420001u)) == Prev::down,
              "lParam bit 30 is the previous state");
        check(k.down('L', Prev::down, 5).repeat, "a down whose message says the key was down already is a repeat");
        check(!k.held(0) && !k.held(256) && !k.down(0, Prev::up, 1).repeat && !k.down(300, Prev::up, 1).repeat,
              "codes outside 1..255 are never held");
    }
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
