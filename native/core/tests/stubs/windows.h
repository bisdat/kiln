// SPDX-License-Identifier: MIT
// Host-only shim for sigdb.cpp's diagnostic APIs. No production build includes this directory.
#pragma once
#include <cstddef>
#include <cstdint>
using HMODULE = void*;
using LPCWSTR = const wchar_t*;
using DWORD = unsigned long;
struct MEMORY_BASIC_INFORMATION { void* BaseAddress; size_t RegionSize; unsigned State, Protect; };
inline constexpr unsigned MEM_COMMIT = 0x1000, PAGE_NOACCESS = 1, PAGE_GUARD = 0x100;
inline constexpr unsigned GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4,
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 2;
inline constexpr unsigned MAX_PATH = 260;
inline MEMORY_BASIC_INFORMATION test_mbi{};
inline bool test_query = false;
inline size_t VirtualQuery(const void*, MEMORY_BASIC_INFORMATION* out, size_t n) {
    if (!test_query) return 0;
    *out = test_mbi;
    return n;
}
// One loaded module a test can place: [test_module_lo, test_module_hi), named relay-mod.dll. None by default.
inline uintptr_t test_module_lo = 0, test_module_hi = 0;
inline bool GetModuleHandleExW(unsigned, LPCWSTR a, HMODULE* out) {
    const auto p = reinterpret_cast<uintptr_t>(a);
    if (p < test_module_lo || p >= test_module_hi) return false;
    *out = reinterpret_cast<HMODULE>(test_module_lo);
    return true;
}
inline DWORD GetModuleFileNameW(HMODULE mod, wchar_t* path, DWORD n) {
    static constexpr wchar_t name[] = L"relay-mod.dll";
    if (!mod || reinterpret_cast<uintptr_t>(mod) != test_module_lo) return 0;
    DWORD i = 0;
    for (; i + 1 < n && name[i]; ++i) path[i] = name[i];
    return i;
}
