// SPDX-License-Identifier: MIT
// kiln.ui: which keys are held, so that one press counts once (input.cpp; tests/key_hold_test.cpp).
//
// A key's down while it is held is a repeat. Windows sends a held key's repeats as more downs. Up to dev.29 a focus loss
// (WM_KILLFOCUS, WM_ACTIVATEAPP 0) forgot every held key, so the next down of a key still held, its own key message
// after its raw record or a repeat, counted as a second press: one press of a window's key opened the window and shut
// it again 40 ms later (Nexus 179900169). Held keys now stay held through a focus loss. The rules, by what an event says:
//  - A key message (WM_KEYDOWN) says the key's previous state (lParam bit 30), as Windows saw it when the key went down,
//    in order: that is the answer. The key message of a raw record already counted (its twin, same message time) is no
//    second event.
//  - A raw record says nothing about before. Its answer is Kiln's held state, which a focus loss leaves uncertain: a
//    key's release can go to another window while the game has no focus. So when the focus comes back, each key's
//    physical state (GetAsyncKeyState) is noted with the time, and laid on the key only with its first event stamped
//    after that time. Events stamped before it were queued while the window's thread had not run yet: GetAsyncKeyState
//    already counts them, so they go by the held state, in their order (a press queued before is a press; an older
//    queued repeat and its release stay a repeat and a release).
//  - A raw record counted a repeat whose key message then says the key was up before it: Kiln had missed the key's
//    release (let go while away, pressed again before the window's thread ran); the press is real and counts then.
#pragma once
#include <atomic>
#include <cstdint>

namespace kui::hold {

// What a key message says about its key just before it (lParam bit 30); a raw record says nothing.
enum class Prev { unknown, up, down };

inline Prev prev_of_lparam(int64_t lparam) { return (lparam >> 30) & 1 ? Prev::down : Prev::up; }

constexpr uint32_t kNone = UINT32_MAX;
// A raw record's key message carries the same message time (a few repeats' records may be queued ahead of their
// messages, so it may trail a little), and comes before the key's release.
constexpr uint32_t kTwinMs = 250;

struct Down {
    bool repeat = false;               // the key was held: not a new press
    uint32_t since_press_ms = kNone;   // since the key's last press (a new one only), kNone: none yet
    uint32_t since_release_ms = kNone; // since its last release Kiln saw, kNone: none since that press
};

// Window thread, except held() (any thread). `t`: the event's message time (GetMessageTime; it wraps), `now_ms`: a clock
// for the log's spans only.
class Keys {
public:
    Down down(uint32_t vk, Prev prev, uint64_t now_ms, uint32_t t) {
        Down d;
        if (!valid(vk)) return d;
        settle_if_newer(vk, t);
        const bool raw = prev == Prev::unknown;
        d.repeat = raw ? held_[vk].load(std::memory_order_relaxed) : prev == Prev::down;
        held_[vk].store(true, std::memory_order_relaxed);
        if (raw) {
            if (pairs_[vk] < 8) ++pairs_[vk];
            raw_at_[vk] = t;
            missed_[vk] = d.repeat;  // its key message may show the key was up before: then the press is real
        } else {
            missed_[vk] = false;
        }
        if (d.repeat) return d;
        if (pressed_at_[vk]) d.since_press_ms = span(now_ms, pressed_at_[vk]);
        if (released_at_[vk] && released_at_[vk] >= pressed_at_[vk]) d.since_release_ms = span(now_ms, released_at_[vk]);
        pressed_at_[vk] = now_ms ? now_ms : 1;
        return d;
    }
    void up(uint32_t vk, uint64_t now_ms, uint32_t t) {
        if (!valid(vk)) return;
        settle_if_newer(vk, t);
        held_[vk].store(false, std::memory_order_relaxed);
        missed_[vk] = false;
        pairs_[vk] = 0;  // a raw record's key message comes before the key's release: none waits past it
        released_at_[vk] = now_ms ? now_ms : 1;
    }
    // A key message's release: the messages of the key's raw records are all through.
    void message_up(uint32_t vk) {
        if (valid(vk)) pairs_[vk] = 0;
    }
    // A key message's down: true when it is the message of a raw record already counted (not a new event).
    bool twin(uint32_t vk, uint32_t t) {
        if (!valid(vk) || !pairs_[vk] || static_cast<uint32_t>(t - raw_at_[vk] + kTwinMs) > 2 * kTwinMs) return false;
        --pairs_[vk];
        return true;
    }
    // The twin's previous state says the key was up before it while its raw record counted a repeat: the press was real
    // (Kiln had missed a release). Once.
    bool missed_press(uint32_t vk, Prev prev) {
        if (!valid(vk) || prev != Prev::up || !missed_[vk]) return false;
        missed_[vk] = false;
        return true;
    }
    bool held(uint32_t vk) const {
        if (!valid(vk)) return false;
        return pend_[vk].load(std::memory_order_acquire) ? pend_down_[vk].load(std::memory_order_relaxed)
                                                         : held_[vk].load(std::memory_order_relaxed);
    }
    // The window lost the keyboard: held keys stay held (their releases may go to the window that has it now). A settle
    // no event took yet is the newest knowledge: it applies now.
    void focus_lost() {
        for (uint32_t vk = 1; vk < 256; ++vk) apply(vk);
        away_ = true;
    }
    // The keyboard is back (WM_SETFOCUS, WM_ACTIVATEAPP 1; also the first focus) at `now_tick` (the message clock): each
    // key's physical state now, laid on it with its first event stamped after now_tick (settle_if_newer).
    template <class IsDown>
    void focus_back(IsDown&& is_down, uint32_t now_tick) {
        settle_at_ = now_tick;
        for (uint32_t vk = 1; vk < 256; ++vk) {
            pend_down_[vk].store(is_down(vk), std::memory_order_relaxed);
            pend_[vk].store(true, std::memory_order_release);
        }
        away_ = false;
    }
    // A focus loss that no focus came back after yet.
    bool away() const { return away_; }

private:
    static bool valid(uint32_t vk) { return vk > 0 && vk < 256; }
    static uint32_t span(uint64_t now, uint64_t then) {
        return now <= then ? 0u : now - then >= kNone ? kNone - 1 : static_cast<uint32_t>(now - then);
    }
    void apply(uint32_t vk) {
        if (!pend_[vk].load(std::memory_order_relaxed)) return;
        held_[vk].store(pend_down_[vk].load(std::memory_order_relaxed), std::memory_order_relaxed);
        pend_[vk].store(false, std::memory_order_release);
    }
    void settle_if_newer(uint32_t vk, uint32_t t) {
        if (pend_[vk].load(std::memory_order_relaxed) && static_cast<int32_t>(t - settle_at_) > 0) apply(vk);
    }
    std::atomic<bool> held_[256] = {};
    std::atomic<bool> pend_[256] = {}, pend_down_[256] = {};  // the state noted at the focus's return, not laid on yet
    uint32_t settle_at_ = 0;
    uint64_t pressed_at_[256] = {}, released_at_[256] = {};
    uint32_t raw_at_[256] = {};
    uint8_t pairs_[256] = {};  // raw downs whose key message has not come yet
    bool missed_[256] = {};    // the last raw down counted a repeat
    bool away_ = false;
};

}  // namespace kui::hold
