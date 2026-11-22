// SPDX-License-Identifier: MIT
// Host test of pad_keys.h: a game controller's buttons as key presses (codes, modifiers, order, what the game keeps
// seeing). input.cpp applies it in its XInputGetState hook.
#include "../pad_keys.h"

#include <cstdio>
#include <vector>
#include <thread>
#include <atomic>

#include "../../../core/abi/services/kiln_ui.h"

namespace {
int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}
struct Ev {
    uint32_t vk;
    bool down;
    uint32_t mods;
    bool operator==(const Ev&) const = default;
};
}  // namespace

int main() {
    using namespace kui::pad;
    check(kVkFirst == KILN_UI_VK_PAD_UP && kVkFirst + 6 == KILN_UI_VK_PAD_LS && kVkFirst + 8 == KILN_UI_VK_PAD_LB &&
              kVkFirst + 0xC == KILN_UI_VK_PAD_A && kVkFirst + 0xF == KILN_UI_VK_PAD_Y && kVkLT == KILN_UI_VK_PAD_LT &&
              kVkRT == KILN_UI_VK_PAD_RT && kModLB == KILN_UI_MOD_PAD_LB && kModRT == KILN_UI_MOD_PAD_RT &&
              (kModsAll & 7u) == 0 && KILN_UI_KEYS_PAD == 2u,
          "codes: 0x200 + the XInput bit, the triggers at 0x210/0x211, pad modifiers apart from Ctrl/Shift/Alt");
    check(is_pad(KILN_UI_VK_PAD_A) && is_pad(KILN_UI_VK_PAD_RT) && !is_pad(0x20A) && !is_pad(0x20B) && !is_pad(0x103) &&
              !is_pad(0x212),
          "is_pad: the 14 buttons and two triggers; XInput's unused bits 10/11, the wheel and above are not");
    check(held(0x1000, 29, 30) == (0x1000u | kBitRT) && held(0x0C00, 0, 0) == 0, "held: a trigger from 30, unused bits dropped");

    Tracker t;
    std::vector<Ev> seen;
    bool keep_a = true;
    auto fn = [&](uint32_t vk, bool down, uint32_t mods) {
        seen.push_back({vk, down, mods});
        return down && vk == KILN_UI_VK_PAD_A && keep_a;
    };
    uint32_t hidden = t.step(held(0x0100, 0, 0), fn);  // LB down
    check(seen == std::vector<Ev>{{KILN_UI_VK_PAD_LB, true, 0}} && hidden == 0, "LB alone: a press without modifiers, not kept");
    seen.clear();
    hidden = t.step(held(0x1100, 0, 0), fn);  // A while LB is held
    check(seen == std::vector<Ev>{{KILN_UI_VK_PAD_A, true, kModLB}} && hidden == 0x1000,
          "LB+A: A with the LB modifier; kept, so A is hidden from the game");
    seen.clear();
    hidden = t.step(held(0x1100, 0, 0), fn);
    check(seen.empty() && hidden == 0x1000, "held: no repeat, A stays hidden while held");
    uint16_t b = 0x1100;
    uint8_t lt = 0, rt = 0;
    hide(hidden, &b, &lt, &rt);
    check(b == 0x0100, "hide: the game sees LB, not the kept A");
    seen.clear();
    hidden = t.step(held(0x0000, 0, 0), fn);  // both up at once
    check(seen == std::vector<Ev>{{KILN_UI_VK_PAD_LB, false, 0}, {KILN_UI_VK_PAD_A, false, 0}} && hidden == 0,
          "releases: both come, nothing kept after A's release");
    seen.clear();
    hidden = t.step(held(0x1000, 200, 0), fn);  // LT and A in one poll: the modifier first
    check(seen == std::vector<Ev>{{KILN_UI_VK_PAD_LT, true, kModLB * 0}, {KILN_UI_VK_PAD_A, true, kModLT}} && hidden == 0x1000,
          "one poll, LT and A: the trigger first, then A as LT+A");
    seen.clear();
    keep_a = false;
    hidden = t.step(held(0x0000, 0, 0), fn);
    hidden = t.step(held(0x1000, 0, 255), fn);
    check(hidden == 0, "a press nobody kept is not hidden");
    uint16_t b2 = 0x1000;
    uint8_t lt2 = 0, rt2 = 255;
    Tracker k2;
    hide(k2.step(held(0x0000, 0, 255), [](uint32_t vk, bool down, uint32_t) { return down && vk == KILN_UI_VK_PAD_RT; }), &b2, &lt2,
         &rt2);
    check(rt2 == 0 && b2 == 0x1000, "a kept trigger reads 0 to the game");
    Controllers controllers;
    std::atomic<int> presses{0}, releases{0};
    auto record = [&](uint32_t, bool down, uint32_t) { (down ? presses : releases).fetch_add(1); return down; };
    std::thread first([&] { controllers.step(0, 0x1000, record); }); first.join();
    std::thread second([&] { controllers.step(0, 0x1000, record); }); second.join();
    check(presses == 1, "controller state: another polling thread sees the same held edge");
    std::thread release([&] { controllers.step(0, 0, record); }); release.join();
    check(releases == 1 && controllers.step(0, 0x1000, record) == 0x1000 && presses == 2,
          "controller state: cross-thread release clears suppression and permits a new press");
    controllers.step(1, 0x1000, record);
    check(presses == 3, "controller state: two controllers keep independent edges");
    controllers.disconnect(0, record);
    check(releases == 2 && controllers.step(0, 0x1000, record) == 0x1000 && presses == 4,
          "disconnect: releases a held binding and reconnecting held is a fresh press");
    controllers.disconnect(0, record);
    controllers.disconnect(0, record);
    check(releases == 3, "disconnect: an already disconnected controller emits no duplicate release");
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
