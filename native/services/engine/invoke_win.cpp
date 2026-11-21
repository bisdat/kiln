// SPDX-License-Identifier: MIT
// Windows bodies of invoke.h's call functions (Kiln-internal). Adapted from ShroudForge (MIT,
// Copyright (c) 2026 bonsaibauer), modules/game-runtime/native/impact/submit_building_prop_spawn.cpp:713-718 (invoke:
// the engine function through a __fastcall pointer under __try / __except) and :837-842 (GetCurrentThreadStackLimits).
// Differences, all safety-only: ShroudForge's handler takes every exception (EXCEPTION_EXECUTE_HANDLER); this one lets a
// stack overflow and the debugger's breakpoint and single-step codes go on to an outer handler, and every exception
// that reaches the frame latches engine calls for the session first (call_fault_seen), so one that goes on and is
// handled further out still leaves the session latched, and one that goes on and is resumed further out (the call then
// returns) is still reported (Return.exception) and answered Uncertain.
// Each function holds only POD in its __try frame (no C++ object with a destructor), as clang-cl requires.
#include <windows.h>

#include <cstring>

#include "invoke.h"

namespace kiln::engine::call {
namespace {
// STATUS_WX86_SINGLE_STEP and STATUS_WX86_BREAKPOINT: the debugger's codes for WOW64 code.
constexpr unsigned long kWx86SingleStep = 0x4000001EUL, kWx86Breakpoint = 0x4000001FUL;

int call_filter(unsigned long code, volatile std::uint32_t* seen) noexcept {
    *seen = static_cast<std::uint32_t>(code);
    call_fault_seen(static_cast<std::uint32_t>(code));
    return filter_decision(code);
}

// For the compare: contain access faults on a page the engine freed or protected; let everything else go on.
int read_filter(unsigned long code) noexcept {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_DATATYPE_MISALIGNMENT ||
                   code == STATUS_GUARD_PAGE_VIOLATION
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

int filter_decision(unsigned long code) noexcept {
    return code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP ||
                   code == kWx86Breakpoint || code == kWx86SingleStep
               ? EXCEPTION_CONTINUE_SEARCH
               : EXCEPTION_EXECUTE_HANDLER;
}

Return invoke(std::uintptr_t entry, const std::uint64_t* args) noexcept {
    using Engine = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                              std::uint64_t, std::uint64_t, std::uint64_t);
    Return r{0, 0, 0};
    volatile std::uint32_t seen = 0;  // written by the filter, also for a code it passes on
    __try {
        r.value = reinterpret_cast<Engine>(entry)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
    } __except (call_filter(GetExceptionCode(), &seen)) {
        r.value = 0;
        r.faulted = 1;
    }
    r.exception = seen;  // nonzero without faulted: passed on, and an outer handler resumed the call
    return r;
}

bool same(std::uintptr_t address, const std::uint8_t* expected, std::size_t size) noexcept {
    if (!address || address > UINTPTR_MAX - size) return false;
    __try {
        return std::memcmp(reinterpret_cast<const void*>(address), expected, size) == 0;
    } __except (read_filter(GetExceptionCode())) {
        return false;
    }
}

bool stack_range(std::uintptr_t& low, std::uintptr_t& high) noexcept {
    ULONG_PTR l = 0, h = 0;
    GetCurrentThreadStackLimits(&l, &h);  // the TEB's stack bounds: no syscall, no lock
    if (!l || !h || l >= h) return false;
    low = l;
    high = h;
    return true;
}
}  // namespace kiln::engine::call
