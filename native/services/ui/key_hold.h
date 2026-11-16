// SPDX-License-Identifier: MIT
// kiln.ui: which keys are held, so that one press counts once (input.cpp; tests/key_hold_test.cpp).
//
// A key's down while it is held is a repeat. Windows sends a held key's repeats as more downs: a key message says
// whether the key was down before (WM_KEYDOWN lParam bit 30); a raw keyboard record does not, so the held state is
// Kiln's own. That state must outlive a focus loss: up to dev.29 a focus loss (WM_KILLFOCUS, WM_ACTIVATEAPP 0) forgot
// every held key, so the next down of a key still held, its own key message after its raw record or a repeat, counted
// as a second press. One press of a window's key opened the window and shut it again 40 ms later (Nexus 179900169).
// A key's release can go to another window while the game has no focus, so it is settled when the focus comes back:
// each key is held exactly when it is down then (GetAsyncKeyState). A key let go while away is a new press next time;
// one held all along is still held.
#pragma once
#include <atomic>
#include <cstdint>

namespace kui::hold {

// What a key message says about its key just before it (lParam bit 30); a raw record says nothing.
enum class Prev { unknown, up, down };

inline Prev prev_of_lparam(int64_t lparam) { return (lparam >> 30) & 1 ? Prev::down : Prev::up; }

constexpr uint32_t kNone = UINT32_MAX;

struct Down {
    bool repeat = false;               // the key was held: not a new press
    uint32_t since_press_ms = kNone;   // since the key's last press (a new one only), kNone: none yet
    uint32_t since_release_ms = kNone; // since its last release Kiln saw, kNone: none since that press
};

// Window thread, except held() (any thread).
class Keys {
public:
    Down down(uint32_t vk, Prev prev, uint64_t now_ms) {
        Down d;
        if (vk == 0 || vk > 255) return d;
        d.repeat = held_[vk].load(std::memory_order_relaxed) || prev == Prev::down;
        held_[vk].store(true, std::memory_order_relaxed);
        if (d.repeat) return d;
        if (pressed_at_[vk]) d.since_press_ms = span(now_ms, pressed_at_[vk]);
        if (released_at_[vk] && released_at_[vk] >= pressed_at_[vk]) d.since_release_ms = span(now_ms, released_at_[vk]);
        pressed_at_[vk] = now_ms ? now_ms : 1;
        return d;
    }
    void up(uint32_t vk, uint64_t now_ms) {
        if (vk == 0 || vk > 255) return;
        held_[vk].store(false, std::memory_order_relaxed);
        released_at_[vk] = now_ms ? now_ms : 1;
    }
    bool held(uint32_t vk) const { return vk > 0 && vk < 256 && held_[vk].load(std::memory_order_relaxed); }
    // The window lost the keyboard: held keys stay held; their releases may go to the window that has it now.
    void focus_lost() { away_ = true; }
    // The keyboard is back (WM_SETFOCUS, WM_ACTIVATEAPP 1): each key is held exactly when it is down now. Also without a
    // loss before (the first focus): a key held through the game's start is held. Returns the keys it changed.
    template <class IsDown>
    int focus_back(IsDown&& is_down) {
        int changed = 0;
        for (uint32_t vk = 1; vk < 256; ++vk) {
            const bool now = is_down(vk);
            if (held_[vk].exchange(now, std::memory_order_relaxed) != now) ++changed;
        }
        away_ = false;
        return changed;
    }
    // A focus loss that no focus came back after yet.
    bool away() const { return away_; }

private:
    static uint32_t span(uint64_t now, uint64_t then) {
        return now <= then ? 0u : now - then >= kNone ? kNone - 1 : static_cast<uint32_t>(now - then);
    }
    std::atomic<bool> held_[256] = {};
    uint64_t pressed_at_[256] = {}, released_at_[256] = {};
    bool away_ = false;
};

}  // namespace kui::hold
