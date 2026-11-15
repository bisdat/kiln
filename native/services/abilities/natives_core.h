// SPDX-License-Identifier: MIT
// kiln.abilities: the decisions of the native thunks (abilities.cpp) with nothing of Windows in them, so the host tests
// (tests/natives_test.cpp) run them with made-up calls. Since 0.2.0-dev.29: synchronous natives.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

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

// ---------------------------------------------------------------------------------------------- attachments ----
// since 0.2.0-dev.29: handlers attached to one vanilla native's calls, filtered here (in the thunk) on the calling
// program and call site, so a call that matches none never enters managed code.
struct Attachment {
    uint64_t id = 0;
    uint32_t hash = 0, program = 0, site = 0;  // program/site 0 = any
    int32_t stage = KILN_NATIVE_BEFORE, priority = 0;
    uint64_t order = 0;                       // registration order (ties)
    KilnNativeWrapFn fn = nullptr;
    void* user = nullptr;
    KilnOwner owner = 0;
    bool active = true;                       // false: its program or site was not found when the world's programs loaded
};

struct AttachSet {
    std::vector<Attachment> before, instead, after;
    bool empty() const { return before.empty() && instead.empty() && after.empty(); }
};

inline bool filter_matches(const Attachment& a, uint32_t program, uint32_t site) {
    return a.active && (a.program == 0 || a.program == program) && (a.site == 0 || a.site == site);
}

// The set a thunk reads: Before by priority high to low, Instead high to low, After low to high; ties in registration
// order.
inline AttachSet build_set(const std::vector<Attachment>& all, uint32_t hash) {
    AttachSet s;
    for (const auto& a : all) {
        if (a.hash != hash) continue;
        (a.stage == KILN_NATIVE_BEFORE ? s.before : a.stage == KILN_NATIVE_INSTEAD ? s.instead : s.after).push_back(a);
    }
    auto high = [](const Attachment& x, const Attachment& y) { return x.priority != y.priority ? x.priority > y.priority : x.order < y.order; };
    auto low = [](const Attachment& x, const Attachment& y) { return x.priority != y.priority ? x.priority < y.priority : x.order < y.order; };
    std::stable_sort(s.before.begin(), s.before.end(), high);
    std::stable_sort(s.instead.begin(), s.instead.end(), high);
    std::stable_sort(s.after.begin(), s.after.end(), low);
    return s;
}

// One Instead per (native, program, site).
inline bool instead_taken(const std::vector<Attachment>& all, uint32_t hash, uint32_t program, uint32_t site) {
    for (const auto& a : all)
        if (a.stage == KILN_NATIVE_INSTEAD && a.hash == hash && a.program == program && a.site == site) return true;
    return false;
}

// One call through the stages: every matching Before; then, if nothing decided the call, the first matching Instead;
// then, if still undecided, the game's native (once); then every matching After. enter(a, stage) runs one attachment;
// original() runs the game's native; decided() says whether a handler answered or the native ran.
template <class Enter, class Original, class Decided>
void run_stages(const AttachSet& set, uint32_t program, uint32_t site, Enter&& enter, Original&& original,
                Decided&& decided, uint64_t* entered, uint64_t* skipped) {
    for (const auto& a : set.before) {
        if (!filter_matches(a, program, site)) { ++*skipped; continue; }
        ++*entered;
        enter(a, KILN_NATIVE_BEFORE);
    }
    if (!decided())
        for (const auto& a : set.instead) {
            if (!filter_matches(a, program, site)) { ++*skipped; continue; }
            ++*entered;
            enter(a, KILN_NATIVE_INSTEAD);
            break;
        }
    if (!decided()) original();
    for (const auto& a : set.after) {
        if (!filter_matches(a, program, site)) { ++*skipped; continue; }
        ++*entered;
        enter(a, KILN_NATIVE_AFTER);
    }
}

// Impact bytecode: words of operands after an opcode (the verifier's table, verify.cpp).
inline int operand_words(uint32_t op) {
    switch (op) {
        case 8: case 9: case 10: case 11: case 21: case 22: case 23: case 24: return 1;  // BR BRT BRF ICONST LOAD GLOAD STORE GSTORE
        case 16: case 18: case 19: return 2;                                              // COPY CALL ECALL
        default: return 0;
    }
}

// The program's code calls `hash` (at `site`; site 0 = at any site): an ECALL site, hash on an instruction start.
inline bool code_calls(const uint32_t* code, size_t n, uint32_t hash, uint32_t site) {
    for (size_t pc = 0; pc < n;) {
        const uint32_t op = code[pc];
        if (op < 1 || op > 30) return false;  // not a program the verifier would take: no match
        const int w = operand_words(op);
        if (op == 19 && pc + 2 < n && code[pc + 2] == hash && (site == 0 || code[pc + 1] == site)) return true;
        pc += 1 + static_cast<size_t>(w);
    }
    return false;
}

}  // namespace kab
