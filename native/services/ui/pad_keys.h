// SPDX-License-Identifier: MIT
// kiln.ui: a game controller's buttons as key presses (key bindings "Pad:LS", "Pad:LB+A", the Keys page's "press a key",
// KeyInput listeners with KeyListenFlags.Pad; 0.2.0-dev.30.5, GitHub issue 119). The rules without XInput or a window:
// input.cpp applies them in its XInputGetState hook for the key subscribers that ask for pad presses (subscribe_key_ex
// with KILN_UI_KEYS_PAD, keys.cpp); tests/pad_keys_test.cpp checks them on the host.
//  - Codes (kiln_ui.h KILN_UI_VK_PAD_*): 0x200 + the button's bit in XINPUT_GAMEPAD.wButtons, and 0x210 / 0x211 for the
//    left and right trigger, pressed from a pull of kTrigger (XINPUT_GAMEPAD_TRIGGER_THRESHOLD). All above the wheel.
//  - Modifiers: LB, RB, LT and RT held when another button goes down (KILN_UI_MOD_PAD_*), the button itself excluded.
//    In one poll that brings several presses, the modifier buttons go first, so "LB and A at the same poll" is LB+A.
//    Releases go before presses.
//  - Keeping: a press a subscriber kept is hidden from the game until that button's release: its bit is cleared, a kept
//    trigger reads 0. The release is always handed over and keeps nothing.
#pragma once
#include <cstdint>
#include <mutex>

namespace kui::pad {

constexpr uint32_t kVkFirst = 0x200, kVkLT = 0x210, kVkRT = 0x211, kVkLast = 0x211;
constexpr uint32_t kModLB = 8, kModRB = 16, kModLT = 32, kModRT = 64, kModsAll = 0x78;
constexpr uint8_t kTrigger = 30;
constexpr uint32_t kBitLT = 1u << 16, kBitRT = 1u << 17;
constexpr uint32_t kBitLB = 0x0100, kBitRB = 0x0200;
constexpr uint32_t kButtons = 0xF3FFu | kBitLT | kBitRT;  // XInput's 14 buttons (bits 10, 11 are unused) and the triggers

constexpr bool is_pad(uint32_t vk) { return vk >= kVkFirst && vk <= kVkLast && (kButtons >> (vk - kVkFirst)) & 1u; }

// The buttons held in one poll: XInput's bits, LT as bit 16 and RT as bit 17.
constexpr uint32_t held(uint16_t buttons, uint8_t lt, uint8_t rt) {
    return (buttons & 0xF3FFu) | (lt >= kTrigger ? kBitLT : 0u) | (rt >= kTrigger ? kBitRT : 0u);
}

constexpr uint32_t mods_of(uint32_t held_mask) {
    return ((held_mask & kBitLB) ? kModLB : 0u) | ((held_mask & kBitRB) ? kModRB : 0u) | ((held_mask & kBitLT) ? kModLT : 0u) |
           ((held_mask & kBitRT) ? kModRT : 0u);
}
constexpr bool is_modifier_bit(uint32_t bit) { return bit == kBitLB || bit == kBitRB || bit == kBitLT || bit == kBitRT; }

// One controller's buttons between polls. step(now, fn) calls fn(vk, down, mods) -> bool (keep: presses only) for every
// button that changed since the last poll and returns the buttons to hide from the game now.
struct Tracker {
    uint32_t prev = 0, kept = 0;

    template <class Fn>
    uint32_t step(uint32_t now, Fn&& fn) {
        now &= kButtons;
        const uint32_t changed = now ^ prev;
        prev = now; // publish the edge before a raw subscriber can re-poll this same controller
        for (uint32_t i = 0; i < 18; ++i) {  // releases first
            const uint32_t bit = 1u << i;
            if (!(changed & bit) || (now & bit)) continue;
            fn(kVkFirst + i, false, mods_of(now & ~bit));
            kept &= ~bit;
        }
        for (int pass = 0; pass < 2; ++pass)  // presses: the modifier buttons, then the rest
            for (uint32_t i = 0; i < 18; ++i) {
                const uint32_t bit = 1u << i;
                if (!(changed & bit) || !(now & bit) || is_modifier_bit(bit) != (pass == 0)) continue;
                if (fn(kVkFirst + i, true, mods_of(now & ~bit))) kept |= bit;
            }
        return kept;
    }
};

// XInput may poll one controller from several threads. Its edges and kept buttons belong to the controller,
// not to a polling thread; serialize its synchronous consumption decisions as well as its state transitions.
class Controllers {
    struct State { std::recursive_mutex mu; Tracker tracker; } states_[4];
public:
    template <class Fn> uint32_t step(uint32_t user, uint32_t now, Fn&& fn) {
        if (user >= 4) return 0;
        auto& state = states_[user];
        std::lock_guard lock(state.mu);
        return state.tracker.step(now, fn);
    }
    template <class Fn> uint32_t disconnect(uint32_t user, Fn&& fn) { return step(user, 0, fn); }
    void reset(uint32_t user) {
        if (user >= 4) return;
        auto& state = states_[user];
        std::lock_guard lock(state.mu);
        state.tracker = {};
    }
};

// Hides the kept buttons from a pad state the game (or Kiln's screens) will read.
inline void hide(uint32_t kept, uint16_t* buttons, uint8_t* lt, uint8_t* rt) {
    *buttons = static_cast<uint16_t>(*buttons & ~(kept & 0xFFFFu));
    if (kept & kBitLT) *lt = 0;
    if (kept & kBitRT) *rt = 0;
}

}  // namespace kui::pad
