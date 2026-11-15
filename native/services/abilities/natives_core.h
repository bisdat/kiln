// SPDX-License-Identifier: MIT
// kiln.abilities: the decisions of the native thunks (abilities.cpp) with nothing of Windows in them, so the host tests
// (tests/natives_test.cpp) run them with made-up calls. Since 0.2.0-dev.29: synchronous natives.
#pragma once
#include <cstdint>
#include <string_view>

#include "services/kiln_abilities.h"

namespace kab {

// The interpreter's error codes a Kiln native answers with (docs/re/impact.md §2; keen::ErrorId).
constexpr uint8_t kErrInternal = 0x11;      // a handler fault: the instance stops with Error, the game goes on
constexpr uint8_t kErrNoConnection = 0x18;  // a required argument is not connected
constexpr uint8_t kErrTypeMismatch = 0x2C;  // an argument's variable has another type or size
constexpr uint8_t kErrUnknownNative = 0x1F; // a Kiln native that was taken off (the interpreter's own "unknown native")
constexpr uint8_t kUndecided = 0xFF;        // KilnNativeCall.out_error before anyone answered

// "<modid>.<Name>": a non-empty mod id, a dot, Name in [A-Za-z0-9_], 90 bytes at most.
inline bool valid_native_name(std::string_view n) {
    const size_t dot = n.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= n.size()) return false;
    for (size_t i = dot + 1; i < n.size(); ++i) {
        const char c = n[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return n.size() <= 90;
}

// The game's native registry is a hash map of capacity 256 whose RegisterNative takes no new key once it holds half of
// that (measured in the lab on server 26430253: the 129th key was refused; 107 keys are the game's own). Kiln refuses
// first, with the reason, and never fills the map (its lookups stop at an empty entry). count/cap as the map says; cap 0
// = unreadable.
inline bool registry_has_room(uint32_t count, uint64_t cap) { return cap != 0 && static_cast<uint64_t>(count) + 1 <= cap / 2; }

// What a sync native's call answers once its fn returned: what fn set, or error 0 value 0 when it set nothing.
inline void sync_result(const KilnNativeCall& c, uint8_t* error, uint8_t* value) {
    if (c.out_error == kUndecided) {
        *error = 0;
        *value = 0;
    } else {
        *error = c.out_error;
        *value = c.out_value;
    }
}

// A fault caught by a thunk's guard. The game's own function runs at most once per call: a wrapped native whose
// original did not run yet runs it now (fail open); one whose original ran keeps the result it gave (the latch);
// a sync native answers Internal; anything else (or a fault inside the game's own native) answers "unknown native".
enum class FaultAnswer { RunOriginal, KeepResult, Internal, Unknown };
enum class ThunkKind { Wrapped, Sync, Other };
inline FaultAnswer fault_answer(ThunkKind kind, bool in_game, bool original_ran, bool have_original) {
    if (kind == ThunkKind::Wrapped && !in_game) {
        if (original_ran) return FaultAnswer::KeepResult;
        if (have_original) return FaultAnswer::RunOriginal;
    }
    if (kind == ThunkKind::Sync && !in_game) return FaultAnswer::Internal;
    return FaultAnswer::Unknown;
}

// The 0.2 ms budget of a sync native (a log line, not a cut-off). ticks/frequency as QueryPerformanceCounter gives.
inline bool over_budget(int64_t ticks, int64_t frequency) {
    return frequency > 0 && ticks * 5000 > frequency;  // ticks / frequency > 0.0002 s
}

}  // namespace kab
