// SPDX-License-Identifier: MIT
// Host-only SEH syntax shim. These tests use valid synthetic memory; faults are injected as C++ exceptions.
#pragma once
#include <chrono>
#include <cstdint>
#define __try try
#define __except(...) catch (...)
#define EXCEPTION_EXECUTE_HANDLER 1
inline uint64_t GetTickCount64() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// native_pack_test.cpp: the few calls native_pack.cpp makes (paths in these tests are ASCII; no kiln-content.dll)
#define CP_UTF8 65001u
#define __fastcall
typedef void* HMODULE;
inline int MultiByteToWideChar(unsigned, unsigned long, const char* s, int n, wchar_t* out, int cap) {
    if (!out) return n;
    const int m = n < cap ? n : cap;
    for (int i = 0; i < m; ++i) out[i] = static_cast<wchar_t>(static_cast<unsigned char>(s[i]));
    return m;
}
inline HMODULE GetModuleHandleW(const wchar_t*) { return nullptr; }
inline void* GetProcAddress(HMODULE, const char*) { return nullptr; }
