// SPDX-License-Identifier: MIT
// Structured-exception guards around game memory walks and subscriber callbacks. No frame here holds a C++ object
// with a destructor, as __try requires.
#include <windows.h>

#include "internal.h"

namespace kg {
bool seh_build_target(uintptr_t target) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(reinterpret_cast<void*>(target), &m, sizeof m) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        target + 16 < target || target + 16 > reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize) return false;
    __try { volatile const uint8_t* p = reinterpret_cast<const uint8_t*>(target); for (int i=0; i<16; ++i) (void)p[i]; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}


bool seh_call(WalkFn f, void* ctx, unsigned long* code) {
    __try {
        f(ctx);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool seh_subscriber(KilnGameplayFn f, KilnGameplayEvent* ev, void* user, uint32_t* verdict, unsigned long* code) {
    __try {
        *verdict = f(ev, user);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct PtrSize {
    void* ptr;
    uint64_t size;
};

bool seh_get_component(uintptr_t world, uint32_t hash, uint32_t entity, void** ptr, uint32_t* size) {
    __try {
        PtrSize out{nullptr, 0};
        fn.get_component(&out, world, hash, entity);
        *ptr = out.ptr;
        *size = static_cast<uint32_t>(out.size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ptr = nullptr;
        *size = 0;
        return false;
    }
}

bool seh_get_transform(uintptr_t world, uint32_t entity, const int64_t** out) {
    __try {
        *out = fn.get_transform(world, entity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return false;
    }
}

}  // namespace kg
