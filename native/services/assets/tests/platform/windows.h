// SPDX-License-Identifier: MIT
// Host-only Win32 shim for kiln.assets' store test: what store.cpp and util.cpp name, no more. No game code runs, and no
// real file is read: CreateFileW opens only the made-up files a test lists in kiln_test_fs (their bytes are a function of
// the offset, so a container of any size costs no memory) and counts every byte read; the wide-string calls copy ASCII.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using DWORD = uint32_t;
using ULONG = uint32_t;
using BOOL = int;
using PUCHAR = unsigned char*;
using HANDLE = void*;
union LARGE_INTEGER {
    int64_t QuadPart;
};
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))
constexpr DWORD GENERIC_READ = 0x80000000u, FILE_SHARE_READ = 1, OPEN_EXISTING = 3, FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr DWORD FILE_BEGIN = 0;
constexpr unsigned CP_UTF8 = 65001;

namespace kiln_test_fs {
struct File {
    std::wstring path;
    uint64_t size = 0;
};
struct Open {
    size_t file = 0;
    uint64_t pos = 0;
    bool closed = false;
};
inline std::vector<File> files;   // what CreateFileW opens
inline std::vector<Open> opens;   // HANDLE = index + 1
inline uint64_t bytes_read = 0;   // every byte ReadFile gave
inline unsigned opened = 0;       // CreateFileW calls that opened a file
inline uint8_t byte_at(uint64_t offset) { return static_cast<uint8_t>((offset * 131u + offset / 251u + 7u) & 0xFF); }
inline Open* open_of(HANDLE h) {
    const auto i = reinterpret_cast<uintptr_t>(h);
    return i >= 1 && i <= opens.size() && !opens[i - 1].closed ? &opens[i - 1] : nullptr;
}
}  // namespace kiln_test_fs

inline void YieldProcessor() {}
inline void Sleep(DWORD) { std::this_thread::yield(); }
inline HANDLE CreateFileW(const wchar_t* path, DWORD, DWORD, void*, DWORD, DWORD, void*) {
    using namespace kiln_test_fs;
    for (size_t i = 0; path && i < files.size(); ++i)
        if (files[i].path == path) {
            opens.push_back({i, 0, false});
            ++opened;
            return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(opens.size()));
        }
    return INVALID_HANDLE_VALUE;
}
inline BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size) {
    const kiln_test_fs::Open* o = kiln_test_fs::open_of(h);
    size->QuadPart = o ? static_cast<int64_t>(kiln_test_fs::files[o->file].size) : 0;
    return o ? 1 : 0;
}
inline BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER* now, DWORD method) {
    kiln_test_fs::Open* o = kiln_test_fs::open_of(h);
    if (!o || method != FILE_BEGIN || to.QuadPart < 0) return 0;
    o->pos = static_cast<uint64_t>(to.QuadPart);
    if (now) now->QuadPart = to.QuadPart;
    return 1;
}
inline BOOL ReadFile(HANDLE h, void* buffer, DWORD n, DWORD* got, void*) {
    using namespace kiln_test_fs;
    Open* o = open_of(h);
    if (got) *got = 0;
    if (!o) return 0;
    const uint64_t size = files[o->file].size;
    const uint64_t take = o->pos >= size ? 0 : std::min<uint64_t>(n, size - o->pos);
    auto* out = static_cast<uint8_t*>(buffer);
    for (uint64_t i = 0; i < take; ++i) out[i] = byte_at(o->pos + i);
    o->pos += take;
    bytes_read += take;
    if (got) *got = static_cast<DWORD>(take);
    return 1;
}
inline BOOL CloseHandle(HANDLE h) {
    if (kiln_test_fs::Open* o = kiln_test_fs::open_of(h)) o->closed = true;
    return 1;
}
inline int MultiByteToWideChar(unsigned, DWORD, const char* s, int n, wchar_t* out, int cap) {
    if (out)
        for (int i = 0; i < n && i < cap; ++i) out[i] = static_cast<unsigned char>(s[i]);
    return n;
}
inline int WideCharToMultiByte(unsigned, DWORD, const wchar_t* s, int n, char* out, int cap, const char*, BOOL*) {
    if (out)
        for (int i = 0; i < n && i < cap; ++i) out[i] = static_cast<char>(s[i]);
    return n;
}
