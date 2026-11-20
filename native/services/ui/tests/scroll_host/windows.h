// SPDX-License-Identifier: MIT
// In-memory Win32 file calls for the production menu-scroll callback test.
#pragma once
#include "../stubs/windows.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

using DWORD = uint32_t;
using HANDLE = void*;
inline HANDLE const INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(-1);
inline constexpr int CP_UTF8 = 65001, GENERIC_READ = 1, GENERIC_WRITE = 2;
inline constexpr int FILE_SHARE_READ = 1, FILE_SHARE_DELETE = 4, OPEN_EXISTING = 3, CREATE_ALWAYS = 2;
inline constexpr int FILE_FLAG_WRITE_THROUGH = 1, MOVEFILE_REPLACE_EXISTING = 1, MOVEFILE_WRITE_THROUGH = 2;
inline constexpr DWORD ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_SHARING_VIOLATION = 32;

namespace fake_disk {
inline std::map<std::wstring, std::string> files;
inline DWORD error = 0;
inline int deletes = 0, writes = 0, fail_deletes = 0;
inline bool fail_writes = false;
struct Open { std::wstring path; };
}
inline DWORD GetLastError() { return fake_disk::error; }
inline HANDLE CreateFileW(const wchar_t* path, int access, int, void*, int, int, void*) {
    if (access == GENERIC_WRITE) {
        if (fake_disk::fail_writes) return INVALID_HANDLE_VALUE;
        fake_disk::files[path].clear();
    } else if (!fake_disk::files.count(path)) {
        fake_disk::error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    return new fake_disk::Open{path};
}
inline bool WriteFile(HANDLE h, const void* p, DWORD size, DWORD* n, void*) {
    ++fake_disk::writes;
    fake_disk::files[static_cast<fake_disk::Open*>(h)->path].append(static_cast<const char*>(p), size);
    *n = size;
    return true;
}
inline bool ReadFile(HANDLE h, void* p, DWORD size, DWORD* n, void*) {
    const auto& s = fake_disk::files[static_cast<fake_disk::Open*>(h)->path];
    *n = std::min<size_t>(size, s.size());
    std::memcpy(p, s.data(), *n);
    return true;
}
inline bool FlushFileBuffers(HANDLE) { return true; }
inline bool CloseHandle(HANDLE h) { delete static_cast<fake_disk::Open*>(h); return true; }
inline bool DeleteFileW(const wchar_t* path) {
    ++fake_disk::deletes;
    if (fake_disk::fail_deletes > 0) {
        --fake_disk::fail_deletes;
        fake_disk::error = ERROR_SHARING_VIOLATION;
        return false;
    }
    if (fake_disk::files.erase(path)) return true;
    fake_disk::error = ERROR_FILE_NOT_FOUND;
    return false;
}
inline bool MoveFileExW(const wchar_t* from, const wchar_t* to, int) {
    auto it = fake_disk::files.find(from);
    if (it == fake_disk::files.end()) return false;
    fake_disk::files[to] = it->second;
    fake_disk::files.erase(it);
    return true;
}
inline int MultiByteToWideChar(int, int, const char* s, int size, wchar_t* out, int) {
    if (out) std::copy(s, s + size, out);
    return size;
}
inline void Sleep(int) {}
[[noreturn]] inline void __fastfail(int) { std::abort(); }
