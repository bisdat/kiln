// SPDX-License-Identifier: MIT
// Host-only stand-ins for the Windows calls of overlay_guard.cpp (overlay_guard_host_test.cpp): its files on the host's
// disk, its timers kept for the test to fire by hand, the environment from the host's. No game, Wine or Windows.
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

using BOOL = int;
using BOOLEAN = unsigned char;
using DWORD = unsigned long;
using HANDLE = void*;
using HMODULE = void*;
using HWND = void*;
using PVOID = void*;
#define VOID void
#define CALLBACK
#define WINAPI
using WAITORTIMERCALLBACK = void (*)(PVOID, BOOLEAN);
using LPTHREAD_START_ROUTINE = DWORD (*)(void*);
constexpr DWORD GENERIC_READ = 0x80000000ul, GENERIC_WRITE = 0x40000000ul;
constexpr DWORD FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, FILE_SHARE_DELETE = 4;
constexpr DWORD CREATE_ALWAYS = 2, OPEN_EXISTING = 3, FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr DWORD ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5;
constexpr DWORD MOVEFILE_REPLACE_EXISTING = 1;
constexpr DWORD WT_EXECUTEDEFAULT = 0, WT_EXECUTEONLYONCE = 8;
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))

namespace guard_host {
inline DWORD last_error = 0;
inline DWORD from_errno() { return errno == ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED; }
struct Timer {
    WAITORTIMERCALLBACK callback;
    PVOID parameter;
    DWORD due, period;
    bool alive;
};
inline std::vector<Timer> timers;  // indices + 1 are the handles
// Runs every live timer that was set to `due` ms (a one-shot one once), as the timer queue would after that time. A
// timer a callback sets meanwhile is not among them: it is due that much later.
inline unsigned fire(DWORD due) {
    unsigned n = 0;
    for (size_t i = 0, count = timers.size(); i < count; ++i) {
        if (!timers[i].alive || timers[i].due != due) continue;
        if (!timers[i].period) timers[i].alive = false;
        ++n;
        timers[i].callback(timers[i].parameter, 1);
    }
    return n;
}
}  // namespace guard_host

inline DWORD GetLastError() { return guard_host::last_error; }
inline HANDLE CreateFileW(const wchar_t* file, DWORD access, DWORD, void*, DWORD disposition, DWORD, HANDLE) {
    const char* mode = disposition == CREATE_ALWAYS ? "wb" : access & GENERIC_WRITE ? "r+b" : "rb";
    FILE* f = std::fopen(std::filesystem::path(file).c_str(), mode);
    if (!f) {
        guard_host::last_error = guard_host::from_errno();
        return INVALID_HANDLE_VALUE;
    }
    return f;
}
inline BOOL ReadFile(HANDLE h, void* buffer, DWORD size, DWORD* read, void*) {
    *read = static_cast<DWORD>(std::fread(buffer, 1, size, static_cast<FILE*>(h)));
    return !std::ferror(static_cast<FILE*>(h));
}
inline BOOL WriteFile(HANDLE h, const void* data, DWORD size, DWORD* written, void*) {
    *written = static_cast<DWORD>(std::fwrite(data, 1, size, static_cast<FILE*>(h)));
    return *written == size;
}
inline BOOL CloseHandle(HANDLE h) { return std::fclose(static_cast<FILE*>(h)) == 0; }
inline BOOL FlushFileBuffers(HANDLE h) { return std::fflush(static_cast<FILE*>(h)) == 0; }
inline BOOL MoveFileExW(const wchar_t* from, const wchar_t* to, DWORD) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);  // replaces `to`, as MOVEFILE_REPLACE_EXISTING
    if (ec) guard_host::last_error = ERROR_ACCESS_DENIED;
    return !ec;
}
inline void Sleep(DWORD) {}
inline BOOL QueueUserWorkItem(LPTHREAD_START_ROUTINE fn, void* context, DWORD) {
    fn(context);  // at once, on the caller's thread
    return 1;
}
inline BOOL CreateTimerQueueTimer(HANDLE* timer, HANDLE, WAITORTIMERCALLBACK callback, PVOID parameter, DWORD due,
                                  DWORD period, DWORD) {
    guard_host::timers.push_back({callback, parameter, due, period, true});
    *timer = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(guard_host::timers.size()));
    return 1;
}
inline BOOL DeleteTimerQueueTimer(HANDLE, HANDLE timer, HANDLE) {
    const auto i = reinterpret_cast<uintptr_t>(timer);
    if (i && i <= guard_host::timers.size()) guard_host::timers[i - 1].alive = false;
    return 1;
}
inline DWORD GetEnvironmentVariableA(const char* name, char* buffer, DWORD size) {
    const char* v = std::getenv(name);
    if (!v) return 0;
    const DWORD n = static_cast<DWORD>(std::strlen(v));
    if (n + 1 > size) return n + 1;
    std::memcpy(buffer, v, n + 1);
    return n;
}
inline HMODULE GetModuleHandleW(const wchar_t*) { return nullptr; }  // no wrapper DLL loaded
inline DWORD GetModuleFileNameW(HMODULE, wchar_t*, DWORD) { return 0; }
