// SPDX-License-Identifier: MIT
// kiln.ui: when a key's down is a new press, so that one press counts once (input.cpp; tests/key_hold_test.cpp).
//
// Up to dev.29 a key's down was a repeat while Kiln's own "down" state said so, and a focus loss (WM_KILLFOCUS,
// WM_ACTIVATEAPP 0) cleared that state: the press's own key message after its raw record then counted as a second press,
// and one F8 opened a window and shut it again 40 ms later (Nexus 179900169). The rule now, per key:
//  - a down is a repeat when no release of the key came since its last down and that down was handled less than the
//    repeat window ago (the clock when the window's thread handled it, not the message's time); every down, repeats
//    too, restarts the window. A key message whose previous-state bit (lParam bit 30) says the key was down is a repeat.
//  - a focus change does not touch this. Message times and GetAsyncKeyState see the queue in another order than the
//    window's thread handles it (a press queued before the focus came back, an older repeat queued behind it): they
//    would swallow a press or make one up.
// The window is longer than the system's key repeat delay (up to 1 s on Windows), so a held key's repeats never count.
// What this gets wrong, rarely: a key held through a focus loss longer than the window counts one press at its next
// repeat; a key let go while the game had no focus (its release went to another window) and pressed again within the
// window counts that press as a repeat.
//
// held() is the other question, "is it down now" (the radial wheel's hold key): the same events, and a key let go while
// the game had no focus is not held once the focus is back (focus_back). That does not change what counts as a press.
#pragma once
#include <atomic>
#include <cstdint>

namespace kui::hold {

// What a key message says about its key just before it (lParam bit 30); a raw record says nothing.
enum class Prev { unknown, up, down };

inline Prev prev_of_lparam(int64_t lparam) { return (lparam >> 30) & 1 ? Prev::down : Prev::up; }

constexpr uint32_t kNone = UINT32_MAX;
constexpr uint32_t kRepeatWindowMs = 1500;

// The repeat window for the system's key repeat delay (SPI_GETKEYBOARDDELAY: 0..3, 250 ms each step from 250 ms):
// the delay and 750 ms, at least kRepeatWindowMs.
inline uint32_t window_for_delay(uint32_t setting) {
    const uint32_t delay = (setting > 3 ? 3 : setting) * 250 + 250;
    return delay + 750 > kRepeatWindowMs ? delay + 750 : kRepeatWindowMs;
}

struct Down {
    bool repeat = false;               // not a new press
    uint32_t since_press_ms = kNone;   // since the key's last press (a new one only), kNone: none yet
    uint32_t since_release_ms = kNone; // since its last release Kiln saw, kNone: none since that press
};

// Window thread, except held() and the window setting (any thread). input_attach sets the window from the render
// thread. `now_ms`: the clock when the event is handled (GetTickCount64).
class Keys {
public:
    void set_window(uint32_t ms) { window_.store(ms, std::memory_order_relaxed); }
    uint32_t window() const { return window_.load(std::memory_order_relaxed); }

    Down down(uint32_t vk, Prev prev, uint64_t now_ms) {
        Down d;
        if (!valid(vk)) return d;
        const uint64_t now = now_ms ? now_ms : 1;
        d.repeat = prev == Prev::down || (open_[vk] && now - last_down_[vk] < window());
        open_[vk] = true;
        last_down_[vk] = now;
        held_[vk].store(true, std::memory_order_relaxed);
        if (d.repeat) return d;
        if (pressed_at_[vk]) d.since_press_ms = span(now, pressed_at_[vk]);
        if (released_at_[vk] && released_at_[vk] >= pressed_at_[vk]) d.since_release_ms = span(now, released_at_[vk]);
        pressed_at_[vk] = now;
        return d;
    }
    void up(uint32_t vk, uint64_t now_ms) {
        if (!valid(vk)) return;
        open_[vk] = false;
        held_[vk].store(false, std::memory_order_relaxed);
        released_at_[vk] = now_ms ? now_ms : 1;
    }
    bool held(uint32_t vk) const { return valid(vk) && held_[vk].load(std::memory_order_relaxed); }
    // The keyboard is back (WM_SETFOCUS, WM_ACTIVATEAPP 1): a key held that is up now was let go while the game had no
    // focus, so it is not held. For held() only; what counts as a press does not change.
    template <class IsDown>
    void focus_back(IsDown&& is_down) {
        for (uint32_t vk = 1; vk < 256; ++vk)
            if (held_[vk].load(std::memory_order_relaxed) && !is_down(vk)) held_[vk].store(false, std::memory_order_relaxed);
    }

private:
    static bool valid(uint32_t vk) { return vk > 0 && vk < 256; }
    static uint32_t span(uint64_t now, uint64_t then) {
        return now <= then ? 0u : now - then >= kNone ? kNone - 1 : static_cast<uint32_t>(now - then);
    }
    std::atomic<uint32_t> window_{kRepeatWindowMs};
    std::atomic<bool> held_[256] = {};  // "is it down now" (held())
    bool open_[256] = {};               // a down without its release yet
    uint64_t last_down_[256] = {}, pressed_at_[256] = {}, released_at_[256] = {};
};

}  // namespace kui::hold
