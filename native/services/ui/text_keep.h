// SPDX-License-Identifier: MIT
// kiln.ui: key presses a mod sends while a Kiln text field has the keyboard stay out of the field (input.cpp;
// tests/text_keep_test.cpp, tests/window_input/harness.cpp).
//
// A module in the game's process that answers a key with a key of its own (SendInput, keybd_event) typed that key into
// Kiln's field too: ShroudLink's client add-on taps B (the game's backpack key, to open the player menu) when its MODS
// key goes down (in its source; players know it as the key that opens its Assistants panel), and with that key on T every
// "t" typed in the Kiln menu's search came out "tb" (HadToRegister, 0.2.0-dev.31.13). The add-on's window procedure sits
// above Kiln's, so it sees the T before Kiln can keep it.
//  - Sent: Kiln's hooks of SendInput and keybd_event write down each key press a module of the game's process sends
//    while a Kiln text field has the keyboard (any thread; sent_key). Releases are not written down: a release types
//    nothing. Nor are characters (KEYEVENTF_UNICODE): those are text, and they type.
//  - Field (the window's thread): the next key message of that key (WM_KEYDOWN, WM_SYSKEYDOWN) takes it, whether or
//    not a raw record (WM_INPUT) came for it: what reaches Kiln's field is the key messages and their characters, and
//    one key's key messages come in the order its presses were made. A raw record may come ahead of the key messages
//    of earlier presses (a pump takes the posted WM_INPUTs first), or not at all (Proton need not deliver every key as
//    one). That press's key message and the characters it makes (WM_CHAR, WM_SYSCHAR, WM_UNICHAR) stay out of Kiln's
//    field; the game, Kiln's key handling and the mods' key listeners get them as before (Kiln keeps every key press
//    from the game while its menu or a screen has the input anyway).
// Input from other programs (an on-screen keyboard, a key remapper, a password manager typing) is not sent from the
// game's process, so it types as always. A sent press whose input never came (another desktop had the input) is
// forgotten after kSentMs; one sent press keeps one press out, so a player's own press of the same key right after it
// types (the sent one comes first: it was queued first).
#pragma once
#include <cstdint>
#include <mutex>

namespace kui::textkeep {

constexpr uint64_t kSentMs = 1000;  // a sent press not seen by then never comes
constexpr int kMaxSent = 32;        // sent presses waiting at once; more are not written down

// KEYEVENTF_* (winuser.h: input.cpp checks they match)
constexpr uint32_t kExtendedKey = 0x1, kKeyUp = 0x2, kUnicode = 0x4, kScanCode = 0x8;

// The key a key press a module sends (a KEYBDINPUT of SendInput, or keybd_event's arguments) comes to the window as, its
// key messages' virtual key; 0 = nothing to write down. A release types nothing. A character (KEYEVENTF_UNICODE) is
// text by intent, never a game key (an on-screen keyboard or a paste typing from the game's process): it types into
// Kiln's field as before. A press sent by scan code, or with no virtual key, is its scan code's key: `scan_vk(code)`
// (MapVirtualKey MAPVK_VSC_TO_VK; 0xE0xx for an extended key). A left or right Shift, Ctrl or Alt is the generic key, as
// the key messages name it (VK_SHIFT, not VK_LSHIFT).
template <class ScanVk>
uint32_t sent_key(uint32_t vk, uint32_t scan, uint32_t flags, ScanVk scan_vk) {
    if (flags & (kKeyUp | kUnicode)) return 0;
    if (vk == 0 || (flags & kScanCode)) vk = scan_vk((flags & kExtendedKey) ? (0xE000u | (scan & 0xFFu)) : scan);
    switch (vk) {
        case 0xA0:  // VK_LSHIFT, VK_RSHIFT: VK_SHIFT
        case 0xA1: return 0x10;
        case 0xA2:  // VK_LCONTROL, VK_RCONTROL: VK_CONTROL
        case 0xA3: return 0x11;
        case 0xA4:  // VK_LMENU, VK_RMENU: VK_MENU
        case 0xA5: return 0x12;
        default: return vk <= 255 ? vk : 0;
    }
}

class Sent {
public:
    // A key press (virtual key 1..255) a module of the game's process sent at `now` (ms).
    void add(uint32_t vk, uint64_t now) {
        if (vk == 0 || vk > 255) return;
        std::lock_guard lk(mu_);
        drop_old(now);
        if (n_ < kMaxSent) marks_[n_++] = {vk, now};
    }
    // A key message's press of `vk` at `now`: true = a sent one (taken: one sent press, one press).
    bool take(uint32_t vk, uint64_t now) {
        if (vk == 0 || vk > 255) return false;
        std::lock_guard lk(mu_);
        drop_old(now);
        for (int i = 0; i < n_; ++i)
            if (marks_[i].vk == vk) {
                for (int j = i + 1; j < n_; ++j) marks_[j - 1] = marks_[j];
                --n_;
                return true;
            }
        return false;
    }
    void clear() {
        std::lock_guard lk(mu_);
        n_ = 0;
    }

private:
    struct Mark {
        uint32_t vk;
        uint64_t at;
    };
    void drop_old(uint64_t now) {
        int k = 0;
        for (int i = 0; i < n_; ++i)
            if (now < marks_[i].at || now - marks_[i].at <= kSentMs) marks_[k++] = marks_[i];
        n_ = k;
    }
    std::mutex mu_;
    Mark marks_[kMaxSent]{};
    int n_ = 0;
};

// The window thread's side: which key messages and characters stay out of Kiln's field.
class Field {
public:
    // A key message's press (WM_KEYDOWN, WM_SYSKEYDOWN); `sent` = Sent::take for it. true = keep this message and the
    // characters it makes out of Kiln's field.
    bool key_down(uint32_t vk, bool sent) {
        chars_vk_ = sent ? vk : 0;
        return sent;
    }
    // A key message's release: the characters of its press have come.
    void key_up(uint32_t vk) {
        if (vk == chars_vk_) chars_vk_ = 0;
    }
    // A character (WM_CHAR, WM_SYSCHAR, WM_UNICHAR): true = it comes of a kept press, keep it out of the field.
    bool character() const { return chars_vk_ != 0; }
    // The window lost the focus.
    void forget() { chars_vk_ = 0; }

private:
    uint32_t chars_vk_ = 0;
};

}  // namespace kui::textkeep
