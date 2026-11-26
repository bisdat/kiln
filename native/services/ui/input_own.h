// SPDX-License-Identifier: MIT
// kiln.ui.input (0.2.0-dev.31.13, mod API 0.2.22; kiln_ui_input.h): a press an owning subscriber takes is its own until
// the release. The rules without a window, XInput or a callback: input_own.cpp applies them for input.cpp (the window's
// keys, mouse buttons and pointer presses) and keys.cpp / the XInput hook (a controller's buttons); tests/input_own_test.cpp
// checks them on the host, and tests/own_parity.json holds the cases the test kit's fake (Fakes/UiInput) replays too.
//
//  - One claim per input: an id is the code (KilnUiKey.vk, 0x01/0x02 for the left/right button, KILN_UI_VK_PAD_* for a
//    pad) and the controller (pads 0..3, 0 otherwise). A new press is offered to the owning subscribers first, in the
//    order they subscribed, before Kiln's other key subscribers, bindings and pointer subscribers; the first that takes it
//    owns it (claim). Nobody else sees that press, its repeats or its release.
//  - transition(): every later down or up of a claimed id. A repeat goes to the owner, a release goes to the owner and
//    ends the claim. Both are kept from the game unless the game saw a down of this press after all (leak(): an input
//    path that could not keep it, such as a raw mouse record Kiln cannot edit, or Alt+F4): then the release reaches the
//    game, so the game is never left holding a button it saw go down.
//  - A new press of a claimed id (its release never reached Kiln) ends the claim as Lost and is a new press.
//  - Ownership ends early: focus loss (end_all, the key and mouse claims go: the release goes to another window; a
//    controller's stay, XInput does not follow the focus), a world change
//    (the owner hears End World; the claim stays as an orphan), unsubscribe or disown (no callback; orphan). An orphan's
//    repeats and release go to nobody and are still kept from the game (it never saw the press); its next new press ends
//    it. So a mod that closes its overlay on a press never lets that press's release reach the game or another mod.
// Thread-safe: the window thread and the controller polling threads share one table. Callbacks never run under its lock:
// operations return the notes to deliver.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace kui::own {

// sources (kiln_ui_input.h KILN_UI_OWN_*)
constexpr uint32_t kKeys = 1, kMouse = 2, kPointer = 4, kPad = 8, kSources = 15;
// note kinds (KILN_UI_OWNED_*)
constexpr uint32_t kPress = 1, kRepeat = 2, kRelease = 3, kEnd = 4;
// why ownership ended without the release (KILN_UI_OWNED_END_*)
constexpr uint32_t kEndNone = 0, kEndFocus = 1, kEndWorld = 2, kEndLost = 3;

constexpr uint32_t kVkLeft = 0x01, kVkRight = 0x02, kVkMiddle = 0x04, kVkX1 = 0x05, kVkX2 = 0x06;
constexpr uint32_t kPadFirst = 0x200, kPadLast = 0x211;

constexpr uint32_t id_of(uint32_t vk, uint32_t controller) { return (vk & 0xFFFFu) | ((controller & 0xFFu) << 16); }
constexpr uint32_t vk_of(uint32_t id) { return id & 0xFFFFu; }
constexpr uint32_t controller_of(uint32_t id) { return id >> 16; }

// The source a code belongs to, 0 for one that cannot be owned (the wheel: a notch has no release; 0; codes Kiln never
// hands out).
constexpr uint32_t source_of(uint32_t vk) {
    if (vk == kVkLeft || vk == kVkRight) return kPointer;
    if (vk == kVkMiddle || vk == kVkX1 || vk == kVkX2) return kMouse;
    if (vk == 0 || vk == 0x03) return 0;  // VK_CANCEL (Ctrl+Break) comes as a key
    if (vk < 0x100) return kKeys;
    if (vk >= kPadFirst && vk <= kPadLast) return kPad;
    return 0;  // the wheel (0x100..0x103)
}

// One callback to make (outside the lock).
struct Note {
    uint64_t sub = 0;
    uint32_t id = 0;
    uint32_t kind = 0;  // kRepeat, kRelease, kEnd
    uint32_t end = kEndNone;
};

// What the caller does with a transition of a claimed id.
struct Decision {
    bool handled = false;  // ownership decided it: no other subscriber, binding or Kiln handler gets it
    bool keep = false;     // keep it from the game
};

class Table {
public:
    // Every later down or up of `id`. `gen`: the client world's generation now (a claim made in another world ended
    // with it). A new press (down && !repeat) of a claimed id is never handled: it ends the claim (Lost) and the caller
    // offers it as a new press.
    Decision transition(uint32_t id, bool down, bool repeat, uint32_t gen, std::vector<Note>* notes) {
        std::lock_guard lk(mu_);
        const auto it = map_.find(id);
        if (it == map_.end()) return {};
        Entry& e = it->second;
        if (!e.orphan && e.gen != gen) {
            add(notes, {e.sub, id, kEnd, kEndWorld});
            e.orphan = true;
        }
        if (down && !repeat) {
            if (!e.orphan) add(notes, {e.sub, id, kEnd, kEndLost});
            erase(it);
            return {};
        }
        Decision d{true, !e.leaked};
        if (!e.orphan) add(notes, {e.sub, id, down ? kRepeat : kRelease, kEndNone});
        if (!down) erase(it);
        return d;
    }

    // `sub` took a new press of `id` (input_own.cpp asks the owners in order).
    void claim(uint32_t id, uint64_t sub, uint32_t gen) {
        std::lock_guard lk(mu_);
        const bool fresh = map_.find(id) == map_.end();
        map_[id] = Entry{sub, gen, false, false};
        if (fresh) count_.fetch_add(1, std::memory_order_release);
    }

    // The game saw a down of this press after all: its release must reach the game.
    void leak(uint32_t id) {
        std::lock_guard lk(mu_);
        if (const auto it = map_.find(id); it != map_.end()) it->second.leaked = true;
    }

    // The game has not seen this id go down although it is held (a claim, an orphan): hide it as held (MK_ bits).
    bool hidden(uint32_t id) const {
        if (!count_.load(std::memory_order_acquire)) return false;
        std::lock_guard lk(mu_);
        const auto it = map_.find(id);
        return it != map_.end() && !it->second.leaked;
    }

    // The subscriber owning `id` now (0: none, or an orphan).
    uint64_t owner(uint32_t id) const {
        std::lock_guard lk(mu_);
        const auto it = map_.find(id);
        return it != map_.end() && !it->second.orphan ? it->second.sub : 0;
    }

    // Focus loss: every owner of a key or mouse button hears End (why), and those claims go (their releases go to another
    // window; one that comes back later reaches the game, which never saw the press: nothing is held). A controller's
    // claims stay as they are: XInput does not follow the window's focus, so the release still comes and is its owner's.
    void end_all(uint32_t why, std::vector<Note>* notes) {
        std::lock_guard lk(mu_);
        for (const auto& [id, e] : sorted()) {
            if (vk_of(id) >= kPadFirst) continue;
            if (!e.orphan) add(notes, {e.sub, id, kEnd, why});
            erase(map_.find(id));
        }
    }

    // The client world changed: owners of claims made in another world hear End World; the claims stay as orphans.
    void end_world(uint32_t gen, std::vector<Note>* notes) {
        std::lock_guard lk(mu_);
        for (auto& [id, e] : sorted())
            if (!e.orphan && e.gen != gen) {
                add(notes, {e.sub, id, kEnd, kEndWorld});
                map_[id].orphan = true;
            }
    }

    // The subscriber went (unsubscribe, its mod unloaded) or gave up (disown; id 0: every one it owns): no callback, its
    // claims stay as orphans until their release. Returns how many it owned.
    size_t disown(uint64_t sub, uint32_t id = 0) {
        std::lock_guard lk(mu_);
        size_t n = 0;
        for (auto& [k, e] : map_)
            if (!e.orphan && e.sub == sub && (id == 0 || k == id)) {
                e.orphan = true;
                ++n;
            }
        return n;
    }

    bool owns(uint64_t sub, uint32_t id) const { return sub != 0 && owner(id) == sub; }

    // The buttons of `controller` claimed by an owner now: bit (code - kPadFirst).
    uint32_t pad_owned(uint32_t controller) const {
        if (!count_.load(std::memory_order_acquire)) return 0;
        std::lock_guard lk(mu_);
        uint32_t bits = 0;
        for (const auto& [id, e] : map_) {
            const uint32_t vk = vk_of(id);
            if (!e.orphan && controller_of(id) == controller && vk >= kPadFirst && vk <= kPadLast) bits |= 1u << (vk - kPadFirst);
        }
        return bits;
    }

    size_t size() const { return count_.load(std::memory_order_acquire); }
    bool empty() const { return size() == 0; }

private:
    struct Entry {
        uint64_t sub = 0;
        uint32_t gen = 0;
        bool orphan = false;  // its owner went or gave up: repeats and the release go to nobody
        bool leaked = false;  // the game saw a down of it: the release reaches the game
    };
    static void add(std::vector<Note>* notes, const Note& n) {
        if (notes) notes->push_back(n);
    }
    void erase(std::unordered_map<uint32_t, Entry>::iterator it) {
        map_.erase(it);
        count_.fetch_sub(1, std::memory_order_release);
    }
    // Notes in id order, whatever the map's (the parity cases compare them).
    std::vector<std::pair<uint32_t, Entry>> sorted() const {
        std::vector<std::pair<uint32_t, Entry>> v(map_.begin(), map_.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return v;
    }
    mutable std::mutex mu_;
    std::unordered_map<uint32_t, Entry> map_;
    std::atomic<size_t> count_{0};
};

// A new press: the owning subscribers whose sources hold its source, in order; `ask(sub)` -> true takes it. The first
// that takes it owns it. Returns the owner (0: nobody took it; the press goes on to Kiln's other handlers).
template <class Subs, class Ask>
uint64_t offer(Table& t, uint32_t id, uint32_t gen, Subs&& subs, Ask&& ask) {
    const uint32_t source = source_of(vk_of(id));
    if (!source) return 0;
    for (auto& s : subs) {
        if (!(s.sources & source)) continue;
        if (ask(s)) {
            t.claim(id, s.handle, gen);
            return s.handle;
        }
    }
    return 0;
}

// MK_* bits (WM_MOUSEMOVE and the other mouse messages' wParam) of the buttons hidden from the game as held.
inline uint32_t hidden_mk(const Table& t) {
    if (t.empty()) return 0;
    uint32_t mk = 0;
    if (t.hidden(id_of(kVkLeft, 0))) mk |= 0x0001;    // MK_LBUTTON
    if (t.hidden(id_of(kVkRight, 0))) mk |= 0x0002;   // MK_RBUTTON
    if (t.hidden(id_of(kVkMiddle, 0))) mk |= 0x0010;  // MK_MBUTTON
    if (t.hidden(id_of(kVkX1, 0))) mk |= 0x0020;      // MK_XBUTTON1
    if (t.hidden(id_of(kVkX2, 0))) mk |= 0x0040;      // MK_XBUTTON2
    return mk;
}

}  // namespace kui::own
