// SPDX-License-Identifier: MIT
// text_keep.h: a key press a mod sends while a Kiln text field has the keyboard stays out of the field (HadToRegister,
// 31.13: ShroudLink's client add-on taps B when its MODS key T goes down, and "t" typed "tb" in the Kiln menu's search).
// Plain logic; the window procedure around it is tests/window_input/harness.cpp (Wine).
#include "../text_keep.h"

#include <cstdio>
#include <string>

namespace {
int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    ++(ok ? g_pass : g_fail);
}

using kui::textkeep::Field;
using kui::textkeep::Sent;
constexpr uint32_t T = 'T', B = 'B', A = 'A', Back = 0x08;

// What the field types of one key's messages, as input.cpp hands them over: the key message (it takes a sent press of
// its key, whether or not a raw record came for it), its character(s), the release.
struct Window {
    Sent sent;
    Field field;
    std::string typed;
    void key(uint32_t vk, char c, uint64_t now) {
        field.key_down(vk, sent.take(vk, now));
        if (c && !field.character()) typed += c;
        field.key_up(vk);
    }
};
}  // namespace

int main() {
    {
        Window w;
        w.key(T, 't', 100);
        w.sent.add(B, 116);  // the add-on's tap, one game frame later
        w.key(B, 'b', 130);
        w.key(T, 't', 400);
        check(w.typed == "tt", "a press a mod sent while the field has the keyboard types nothing (\"t\" stays \"t\")");
    }
    {
        Window w;
        w.key(B, 'b', 100);
        w.key(A, 'a', 200);
        check(w.typed == "ba", "typed keys type, with nothing sent");
    }
    {
        Window w;
        w.sent.add(B, 100);
        w.key(B, 'b', 110);
        w.key(B, 'b', 300);
        check(w.typed == "b", "one sent press keeps one press out: the player's next B types");
    }
    {
        Window w;
        w.sent.add(B, 100);
        w.key(B, 'b', 100 + kui::textkeep::kSentMs + 1);
        check(w.typed == "b", "a sent press whose input never came is forgotten after kSentMs: a later B types");
    }
    {
        Window w;  // two Bs in a row: the sent one and the player's (their raw records, if any, ahead of them)
        w.sent.add(B, 100);
        w.key(B, 'b', 110);
        w.key(B, 'b', 111);
        check(w.typed == "b", "a sent B and the player's B right behind it: one types");
    }
    {
        Window w;
        w.sent.add(Back, 100);  // a sent Backspace (no character) edits nothing either
        check(w.field.key_down(Back, w.sent.take(Back, 110)), "a sent key without a character: its key message stays out of the field");
        w.field.key_up(Back);
        check(!w.field.key_down(Back, w.sent.take(Back, 300)), "the player's own Backspace after it edits");
    }
    {
        Window w;  // a tap sent as one SendInput call, a typed T behind it
        w.sent.add(B, 100);
        check(w.field.key_down(B, w.sent.take(B, 110)) && w.field.character(), "the sent B's key message and character stay out");
        w.field.key_up(B);
        check(!w.field.key_down(T, w.sent.take(T, 111)) && !w.field.character(), "the typed T behind it types");
    }
    {
        Window w;
        w.sent.add(B, 100);
        w.field.key_down(B, w.sent.take(B, 110));
        w.field.forget();  // the window lost the focus (before the sent B's character)
        check(!w.field.character(), "a focus loss forgets what was kept");
        w.sent.add(B, 200);
        w.sent.clear();
        check(!w.field.key_down(B, w.sent.take(B, 210)), "and Sent::clear forgets the sent presses");
    }
    {
        Sent s;
        for (int i = 0; i < kui::textkeep::kMaxSent + 8; ++i) s.add(B, 100);
        int n = 0;
        while (s.take(B, 120)) ++n;
        check(n == kui::textkeep::kMaxSent, "at most kMaxSent sent presses wait at once");
        check(!s.take(300, 120) && !s.take(0, 120), "keys outside 1..255 are never sent presses");
    }
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
