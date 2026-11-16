// SPDX-License-Identifier: MIT
// key_hold.h: one press counts once, also across a focus loss (GitHub #39, Nexus 179900169), and the focus's return is
// settled in the order of the queued events. Plain logic; the window procedure around it is
// tests/window_input/harness.cpp (Wine).
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
    {
        kui::hold::Keys k;
        check(!k.down(F8, kRaw, 1000, 1000).repeat, "a first down is a press");
        check(k.held(F8), "and the key is held");
        check(k.down(F8, kRaw, 1500, 1500).repeat, "a raw down while held is a repeat");
        k.up(F8, 1600, 1600);
        check(!k.held(F8), "a release ends the hold");
        const auto d = k.down(F8, Prev::up, 1700, 1700);
        check(!d.repeat && d.since_press_ms == 700 && d.since_release_ms == 100,
              "the next down is a press, 700 ms after the last press and 100 ms after the release");
    }
    {
        kui::hold::Keys k;  // the 40 ms case: the press's raw record, a focus loss, then the same press's key message
        k.down(F8, kRaw, 10, 10);
        k.focus_lost();
        check(k.held(F8) && k.away(), "a focus loss keeps the held key held");
        check(k.twin(F8, 10) && !k.missed_press(F8, Prev::up), "the same press's key message is its twin, no second press");
        check(!k.twin(F8, 10), "a raw record has one key message");
    }
    {
        kui::hold::Keys k;  // held all through a focus loss and back
        k.down(J, kRaw, 10, 10);
        k.focus_lost();
        k.focus_back([](uint32_t vk) { return vk == 'J'; }, 500);
        check(!k.away() && k.held(J), "focus back with J still down: J held");
        check(k.down(J, kRaw, 800, 800).repeat, "J's next raw record, after the return, is a repeat");
    }
    {
        kui::hold::Keys k;  // let go while the game had no focus
        k.down(J, kRaw, 10, 10);
        k.focus_lost();
        k.focus_back([](uint32_t) { return false; }, 500);
        check(!k.held(J), "focus back with J up: J no longer held");
        const auto d = k.down(J, kRaw, 900, 900);
        check(!d.repeat && d.since_release_ms == kui::hold::kNone && d.since_press_ms == 890,
              "J's next raw down, after the return, is a press, with no release seen since the last one");
    }
    {
        kui::hold::Keys k;  // pressed while away and still down: its downs after the return are repeats
        k.focus_lost();
        k.focus_back([](uint32_t vk) { return vk == 'K'; }, 500);
        check(k.held('K') && k.down('K', kRaw, 600, 600).repeat, "a key went down while away and is held: its raw downs are repeats");
    }
    {
        kui::hold::Keys k;  // A: F7 pressed after the focus came back, before the window's thread ran: its queued down
        constexpr uint32_t F7 = 0x76;  // is stamped before the return's sample, which already sees F7 down
        k.focus_lost();
        k.focus_back([](uint32_t vk) { return vk == 0x76; }, 500);
        check(!k.down(F7, kRaw, 520, 480).repeat, "A: a raw press queued before the return's sample is a press");
        check(k.twin(F7, 480) && !k.missed_press(F7, Prev::up), "A: its key message is its twin");
        check(k.down(F7, kRaw, 900, 900).repeat, "A: its repeat after the sample is a repeat");
        kui::hold::Keys m;  // A with key messages only
        m.focus_lost();
        m.focus_back([](uint32_t vk) { return vk == 0x75; }, 500);
        check(!m.down(0x75, Prev::up, 520, 480).repeat, "A: a key message press queued before the sample is a press");
    }
    {
        kui::hold::Keys k;  // B: J held through the return; its repeat and release queued before the sample, which sees J up
        k.down(J, kRaw, 10, 10);
        k.focus_lost();
        k.focus_back([](uint32_t) { return false; }, 500);
        check(k.down(J, kRaw, 520, 450).repeat, "B: an older queued raw repeat stays a repeat");
        k.up(J, 520, 470);
        check(!k.held(J), "B: and its queued release ends the hold");
        check(!k.down(J, kRaw, 900, 900).repeat, "B: the next down after the return is a press");
    }
    {
        kui::hold::Keys k;  // J held at the loss, let go while away, pressed again before the window's thread ran: the raw
        k.down(J, kRaw, 10, 10);  // record cannot tell; its key message's previous state can
        k.focus_lost();
        k.focus_back([](uint32_t vk) { return vk == 'J'; }, 500);
        check(k.down(J, kRaw, 520, 480).repeat, "re-pressed while away: the raw record alone counts a repeat");
        check(k.twin(J, 480) && k.missed_press(J, Prev::up), "its key message says J was up before: the press was real");
        check(!k.missed_press(J, Prev::up), "said once");
        k.down(J, kRaw, 600, 600);
        check(k.twin(J, 600) && !k.missed_press(J, Prev::down), "an autorepeat's message (previous state down) changes nothing");
    }
    {
        kui::hold::Keys k;  // twins match by message time
        k.down(J, kRaw, 10, 1000);
        check(!k.twin(J, 2000), "a key message a second after the raw record is not its twin");
        check(k.twin(J, 1030), "one 30 ms after it is (records queued ahead of their messages)");
        k.down(J, kRaw, 10, 0xFFFFFFF0u);
        check(k.twin(J, 0x10u), "the message clock wraps");
        k.down(J, kRaw, 10, 3000);
        k.up(J, 20, 3010);
        check(!k.twin(J, 3020), "a raw record without a key message: its release ends the wait (a later key message is new)");
    }
    {
        kui::hold::Keys k;  // a key message's previous-state bit (lParam bit 30)
        check(kui::hold::prev_of_lparam(0x00420001) == Prev::up && kui::hold::prev_of_lparam(0x40420001) == Prev::down &&
                  kui::hold::prev_of_lparam(static_cast<int64_t>(0xC0420001u)) == Prev::down,
              "lParam bit 30 is the previous state");
        check(k.down('L', Prev::down, 5, 5).repeat, "a down whose message says the key was down already is a repeat");
        k.down('M', kRaw, 5, 5);
        k.message_up('M');
        check(!k.twin('M', 5), "a key message's release: no twin waits");
        check(!k.held(0) && !k.held(256) && !k.down(0, Prev::up, 1, 1).repeat && !k.down(300, Prev::up, 1, 1).repeat,
              "codes outside 1..255 are never held");
    }
    {
        kui::hold::Keys k;  // a settle no event took applies at the next loss
        k.down(J, kRaw, 10, 10);
        k.focus_lost();
        k.focus_back([](uint32_t) { return false; }, 500);
        k.focus_lost();
        check(!k.held(J) && !k.down(J, kRaw, 20, 20).repeat, "the return's state stands at the next loss");
    }
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
