// SPDX-License-Identifier: MIT
// One narrowly focused window-thread -> UI-thread chat key router.
// Preserves PR #63's history behaviour; completion is inert until the client
// explicitly supplies live suggestions for the focused native chat editor.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

namespace kui::chat_keys {

enum class Action : uint8_t {
    Older = 1, Newer = 2,
    CompleteNext = 3, CompletePrevious = 4,
    SelectOlder = 5, SelectNewer = 6, Dismiss = 7
};

// What ends a history browse: actual typed characters, not the transient
// before/after difference of the game's own chat widget.
constexpr bool inserts_text(uint32_t ch) { return (ch >= 0x20 && ch != 0x7f) || ch == '\t'; }
// Keys changing the text without a character. 1=Ctrl, 2=Shift, 4=Alt.
constexpr bool edits_text(uint32_t vk, uint32_t mods) {
    constexpr uint32_t kBack = 0x08, kDelete = 0x2E, kInsert = 0x2D, kCtrl = 1, kShift = 2;
    return vk == kBack || vk == kDelete || ((mods & kShift) && vk == kInsert) ||
           ((mods & kCtrl) && (vk == 'V' || vk == 'X' || vk == 'Z' || vk == 'Y'));
}

class Router {
public:
    void focus(bool yes) {
        if (!yes) {
            active_.store(false, std::memory_order_release);
            std::lock_guard lock(mu_);
            pending_.clear();
            tab_character_.store(false, std::memory_order_release);
            completion_.store(0, std::memory_order_release);
        } else {
            active_.store(true, std::memory_order_release);
        }
    }

    bool focused() const { return active_.load(std::memory_order_acquire); }

    // Called from the UI thread after validated native chat input readback.
    // The lower bit enables completion. Never consume Tab with no live
    // candidates unless its list was already activated. Native chat keeps its
    // ordinary Tab behaviour when no candidates exist.
    void completion(bool enabled, bool suggestions, bool list_open) {
        constexpr uint8_t kEnabled = 1, kSuggestions = 2, kList = 4;
        const uint8_t bits = enabled ? static_cast<uint8_t>(kEnabled |
                                (suggestions ? kSuggestions : 0) | (list_open ? kList : 0)) : 0;
        completion_.store(bits, std::memory_order_release);
        if (!enabled) tab_character_.store(false, std::memory_order_release);
    }

    // History and completion are independent user options. Preserve history
    // as enabled in this standalone router's original host tests by default.
    void history(bool enabled) { history_.store(enabled, std::memory_order_release); }

    // Raw/legacy keydown is deduplicated by input.cpp. This method MUST never
    // swallow key releases; native game input needs those to avoid stuck keys.
    bool key(uint32_t vk, uint32_t mods, bool down) {
        if (!down || !focused()) return false;
        constexpr uint32_t kUp = 0x26, kDown = 0x28, kTab = 0x09, kEscape = 0x1b;
        constexpr uint32_t kCtrl = 1, kShift = 2;
        constexpr uint8_t kEnabled = 1, kSuggestions = 2, kList = 4;
        const uint8_t comp = completion_.load(std::memory_order_acquire);
        const bool completion_enabled = (comp & kEnabled) != 0;
        const bool list_open = completion_enabled && (comp & kList) != 0;
        const bool has_candidates = completion_enabled && (comp & kSuggestions) != 0;
        const bool history_enabled = history_.load(std::memory_order_acquire);
        Action action{};

        if (vk == kTab && (mods == 0 || mods == kShift) && (has_candidates || list_open)) {
            action = mods == kShift ? Action::CompletePrevious : Action::CompleteNext;
        } else if (vk == kEscape && mods == 0 && list_open) {
            action = Action::Dismiss;
        } else if (vk == kUp || vk == kDown) {
            if (list_open && mods == 0) {
                action = vk == kUp ? Action::SelectOlder : Action::SelectNewer;
            } else if (history_enabled && (mods == 0 || (list_open && mods == kCtrl))) {
                action = vk == kUp ? Action::Older : Action::Newer;
            } else {
                return false;
            }
        } else {
            return false;
        }

        std::lock_guard lock(mu_);
        if (!active_.load(std::memory_order_acquire) || pending_.size() >= 32) return false;
        pending_.push_back(action);
        if (vk == kTab) tab_character_.store(true, std::memory_order_release);
        return true;
    }

    // The game accepts a literal '\t' in WM_CHAR. Only swallow one that is
    // paired with an accepted Tab keydown in this focused chat input.
    bool take_tab_character() {
        if (!focused()) return false;
        return tab_character_.exchange(false, std::memory_order_acq_rel);
    }

    // Called only on the game's UI thread from the scoped TextInputProcess.
    bool take(Action& out) {
        std::lock_guard lock(mu_);
        if (pending_.empty() || !active_.load(std::memory_order_acquire)) return false;
        out = pending_.front();
        pending_.pop_front();
        return true;
    }

private:
    std::atomic<bool> active_{false};
    std::atomic<bool> history_{true};
    std::atomic<uint8_t> completion_{0};
    std::atomic<bool> tab_character_{false};
    std::mutex mu_;
    std::deque<Action> pending_;
};

} // namespace kui::chat_keys
