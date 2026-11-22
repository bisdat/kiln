// SPDX-License-Identifier: MIT
// kiln.routines: a plan as the service holds it. Read from the wire document (format 1, kiln_routines.h) with every
// check the contract names, immutable afterwards: the hooks read it on the game's worker threads.
// No game and no Windows in here: it builds and runs on the host (tests/).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kroutines {

enum class Kind : uint8_t { Vanilla, Idle, Roam, Sleep, Sit, Inspect, None = 0xff };
const char* kind_name(Kind k);

struct Window {
    int32_t start, end;
    bool all_day;
    int32_t priority;
    Kind task;
};

struct Plan {
    std::string key;   // <mod>:<name>
    std::string mod;   // the part before ':'
    uint32_t version = 0;
    std::string hash;  // sha256 of the canonical document, lower-case hex
    std::vector<Window> windows;
    Kind fallback = Kind::Vanilla;

    // The window of a minute of the day (0..1439): the highest priority that covers it, the first written among
    // equals; -1: none (the fallback). No allocation, no lock: called in the hooks.
    int32_t select(int32_t minute) const noexcept;
    Kind task_at(int32_t window) const noexcept { return window < 0 ? fallback : windows[static_cast<size_t>(window)].task; }
};

constexpr int32_t kMinutesPerDay = 1440;
constexpr int64_t kNsPerMinute = 60'000'000'000;

// floor(ns / one minute) modulo 1440, also for a negative time.
int32_t minute_of(int64_t now_ns) noexcept;
bool covers(const Window& w, int32_t minute) noexcept;

// The canonical document of a plan's content, and its hash.
std::string canonical(uint32_t version, const std::vector<Window>& windows, Kind fallback);
std::string sha256_hex(std::string_view bytes);

// Reads a wire document. False with `problem` (plain words) when it is not a sound plan.
bool parse_plan(std::string_view json, Plan& out, std::string& problem);

}  // namespace kroutines
