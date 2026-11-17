// SPDX-License-Identifier: MIT
// Chat input's narrow window-thread -> UI-thread handoff. The native edit hook,
// not the HUD screen number, must assert focus. With focus off, input is vanilla.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

namespace kui::chat_keys {

enum class Action : uint8_t { Older = 1, Newer = 2 };

class Router {
public:
    void focus(bool yes) {
        if (!yes) {
            active_.store(false, std::memory_order_release);
            std::lock_guard lock(mu_);
            pending_.clear();
        } else {
            active_.store(true, std::memory_order_release);
        }
    }

    bool focused() const { return active_.load(std::memory_order_acquire); }

    // WndProc deduplicates the raw and legacy key messages before calling this.
    // Never swallow key releases; the game must not retain a stuck key.
    bool key(uint32_t vk, uint32_t mods, bool down) {
        if (!focused() || mods != 0 || (vk != 0x26 && vk != 0x28)) return false;
        if (down) {
            std::lock_guard lock(mu_);
            if (pending_.size() < 32) pending_.push_back(vk == 0x26 ? Action::Older : Action::Newer);
        }
        return down;
    }

    // Called only on the game's UI thread by a scoped chat edit hook.
    bool take(Action& out) {
        std::lock_guard lock(mu_);
        if (pending_.empty() || !active_.load(std::memory_order_acquire)) return false;
        out = pending_.front();
        pending_.pop_front();
        return true;
    }

private:
    std::atomic<bool> active_{false};
    std::mutex mu_;
    std::deque<Action> pending_;
};

} // namespace kui::chat_keys
