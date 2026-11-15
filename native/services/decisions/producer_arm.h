// SPDX-License-Identifier: MIT
// kiln.decisions: when a producer's game hook goes in, with nothing of the game in it (tests/producer_arm_test.cpp).
//
// Since 0.2.0-dev.29 nothing is hooked for a player whose mods do not use decisions: at the service's start a producer
// only resolves what its hook needs (Ready); the hook goes in at the kind's first prepare_kind (a mod subscribed to its
// event), its first interest, or (death) the first native guard, and then stays for the process (Armed). An install
// that fails at that moment refuses the kind for the process (Refused, with the reason): its capability row is lowered
// and the subscribing mod gets the row's refusal. Nothing is retried.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "services/kiln_caps.h"

namespace kiln::decisions {

class ProducerArm {
public:
    enum class State : uint8_t { Unresolved, Ready, Armed, Refused };

    // The start found what the hook needs; it goes in at the first arm().
    void ready() {
        std::lock_guard lock(m_);
        if (state_ == State::Armed) return;
        state_ = State::Ready;
        why_.clear();
        reason_ = KILN_CAP_REASON_NONE;
    }
    // The start (or an install) says the kind cannot be held, and why.
    void refuse(const std::string& why, uint32_t reason) {
        std::lock_guard lock(m_);
        if (state_ == State::Armed) return;
        state_ = State::Refused;
        why_ = why;
        reason_ = reason;
    }

    // Puts the hook in now unless it is in already. `install(std::string& error)` is called at most once in the
    // process, under this producer's lock (another arm() waits for it); false: refused with `error`, FAULTED. Any
    // thread. True: the hook is in.
    template <class Install>
    bool arm(Install&& install) {
        if (armed_.load(std::memory_order_acquire)) return true;
        std::lock_guard lock(m_);
        if (state_ == State::Armed) return true;
        if (state_ != State::Ready) return false;
        std::string error;
        if (!install(error)) {
            state_ = State::Refused;
            why_ = error.empty() ? std::string("the hook could not be installed") : error;
            reason_ = KILN_CAP_REASON_FAULTED;
            return false;
        }
        state_ = State::Armed;
        armed_.store(true, std::memory_order_release);
        return true;
    }

    // Lock free: the hook is in (a hook reads this first).
    bool armed() const { return armed_.load(std::memory_order_relaxed); }
    // Ready or Armed: the kind can be held (its supported_kinds bit).
    bool usable() const {
        std::lock_guard lock(m_);
        return state_ == State::Ready || state_ == State::Armed;
    }
    State state() const {
        std::lock_guard lock(m_);
        return state_;
    }
    std::string why() const {
        std::lock_guard lock(m_);
        return why_;
    }
    uint32_t reason() const {
        std::lock_guard lock(m_);
        return reason_;
    }

private:
    mutable std::mutex m_;
    State state_ = State::Unresolved;
    std::atomic<bool> armed_{false};
    std::string why_ = "the producer did not start";
    uint32_t reason_ = KILN_CAP_REASON_SERVICE_MISSING;
};

}  // namespace kiln::decisions
