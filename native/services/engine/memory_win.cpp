// SPDX-License-Identifier: MIT
// Windows bodies of memory.h. Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer),
// modules/game-runtime/native/engine-context/resolve_engine_owners.cpp:16-20, lock_scene_access.cpp:21-63 and
// ecs/read_entity_membership.cpp:52-58.
// Each function holds only POD in its __try frame (no C++ object with a destructor), as clang-cl requires.
#include <windows.h>

#include <cstring>

#include "memory.h"

namespace kiln::engine::mem {
namespace {
// Contain access faults on a page the engine freed or protected; let everything else (stack overflow, breakpoints,
// C++/Rust exceptions of our own) go on, as no engine read raises them.
int filter(unsigned long code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_DATATYPE_MISALIGNMENT ||
                   code == STATUS_GUARD_PAGE_VIOLATION
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

bool read_span(std::uintptr_t address, void* out, std::size_t size) {
    if (!address || address > UINTPTR_MAX - size) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (filter(GetExceptionCode())) {
        return false;
    }
}

bool write_span(std::uintptr_t address, const void* data, std::size_t size) {
    if (!address || address > UINTPTR_MAX - size) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(address), data, size);
        return true;
    } __except (filter(GetExceptionCode())) {
        return false;
    }
}

// ShroudForge tries one CAS (lock_scene_access.cpp:21-29); here the CAS is retried while the word stays Shared, so two
// Kiln threads taking it Shared at once do not refuse each other. Never retried against an exclusive holder, never
// a pause or a wait: lock-free, at most kSharedTries attempts.
constexpr int kSharedTries = 128;
bool try_shared(std::uintptr_t address) {
    if (!address || address % alignof(LONG)) return false;
    __try {
        auto* lock = reinterpret_cast<volatile LONG*>(address);
        for (int i = 0; i < kSharedTries; ++i) {
            const LONG current = *lock;
            if (current < 0 || current >= 0x7fffffff) return false;
            if (InterlockedCompareExchange(lock, current + 1, current) == current) return true;
        }
        return false;
    } __except (filter(GetExceptionCode())) {
        return false;
    }
}

bool try_exclusive(std::uintptr_t address) {
    if (!address || address % alignof(LONG)) return false;
    __try {
        return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(address), static_cast<LONG>(0x80000000u), 0) == 0;
    } __except (filter(GetExceptionCode())) {
        return false;
    }
}

void release_shared(std::uintptr_t address) noexcept {
    __try {
        InterlockedDecrement(reinterpret_cast<volatile LONG*>(address));
    } __except (filter(GetExceptionCode())) {
    }
}

void release_exclusive(std::uintptr_t address) noexcept {
    __try {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(address), 0);
    } __except (filter(GetExceptionCode())) {
    }
}

bool call_tracker(std::uintptr_t fn, std::uintptr_t manager, std::uintptr_t context, std::uintptr_t entity,
                  std::uint16_t index) {
    using Callback = void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uint16_t, std::uint8_t);
    __try {
        reinterpret_cast<Callback>(fn)(manager, context, entity, index, 0);
        return true;
    } __except (filter(GetExceptionCode())) {
        return false;
    }
}

bool boost_priority(int* saved) {
    const HANDLE self = GetCurrentThread();
    const int now = GetThreadPriority(self);
    if (now == THREAD_PRIORITY_ERROR_RETURN || now >= THREAD_PRIORITY_ABOVE_NORMAL) return false;
    if (!SetThreadPriority(self, THREAD_PRIORITY_ABOVE_NORMAL)) return false;
    *saved = now;
    return true;
}

void restore_priority(int saved) noexcept { SetThreadPriority(GetCurrentThread(), saved); }

}  // namespace kiln::engine::mem
