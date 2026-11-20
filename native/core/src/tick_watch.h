// SPDX-License-Identifier: MIT
// A watch over the core's wait for a world's jobs before its STOPPING (tick.cpp hk_world_destroy, since 0.2.0-dev.29).
// The wait is the game's own WaitWorldJobs and has no deadline: Kiln must not end it early (the destroy would then free
// the world under jobs still running). If it does not return, kiln.log must say so while it lasts: a watchdog thread that
// only reads this state writes "still waiting" lines, the first after kFirstMs and then each time twice as late, naming
// the world, how long it has waited and that the game's own job queue of that world is still busy. It never touches the
// wait or the world. Pure state with the time passed in, so a host test drives it.
#pragma once
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace kiln::tick {

class WaitWatch {
public:
    static constexpr double kFirstMs = 5000;  // the first "still waiting" line; then 10 s, 20 s, 40 s, ...

    // The wait begins (the destroying thread, right before WaitWorldJobs).
    void arm(const char* kind_name, uintptr_t world, double now_ms) {
        std::lock_guard lock(m_);
        armed_ = true;
        kind_ = kind_name ? kind_name : "?";
        world_ = world;
        start_ = now_ms;
        next_ = now_ms + kFirstMs;
        lines_ = 0;
    }
    // The wait returned. Lines said while it lasted (for the completion line).
    uint32_t disarm() {
        std::lock_guard lock(m_);
        armed_ = false;
        return lines_;
    }
    // The watchdog's poll: true with the line when one is due at `now_ms`.
    bool due(double now_ms, std::string* line) {
        std::lock_guard lock(m_);
        if (!armed_ || now_ms < next_) return false;
        const double waited = now_ms - start_;
        next_ = start_ + 2 * waited;
        ++lines_;
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "tick: %s world 0x%llx: still waiting for world jobs before STOPPING (WaitWorldJobs) after %.0f s: "
                      "the game's own job queue of this world is still busy; nothing is freed while it waits",
                      kind_.c_str(), static_cast<unsigned long long>(world_), waited / 1000.0);
        if (line) *line = buf;
        return true;
    }
    bool armed() {
        std::lock_guard lock(m_);
        return armed_;
    }

private:
    std::mutex m_;
    bool armed_ = false;
    std::string kind_;
    uintptr_t world_ = 0;
    double start_ = 0, next_ = 0;
    uint32_t lines_ = 0;
};

}  // namespace kiln::tick
