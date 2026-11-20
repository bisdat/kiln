// SPDX-License-Identifier: MIT
// kiln.ui: a working copy of one function of the game's exe, made at run time (menu_footer.cpp uses it for the main
// menu's footer). Windows glue around code_clone.h: the function's bytes from the exe file on disk (no other module's
// patches), memory within rel32 reach of the image, the copy's unwind information. tests/code_copy_wine/ runs it under
// Wine on a function of its own exe.
#pragma once
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "code_clone.h"

namespace code_copy {

// n bytes of the exe file's code at `rva` (the file on disk: no other module's patches).
inline bool read_file_code(uint32_t rva, uint32_t n, std::vector<uint8_t>* out, std::string* why) {
    wchar_t path[2048];
    const DWORD len = GetModuleFileNameW(nullptr, path, 2048);
    if (!len || len >= 2048) return *why = "the exe's path is not known", false;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return *why = "the exe file cannot be read", false;
    auto read_at = [&](uint64_t off, void* dst, DWORD size) {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(off);
        DWORD got = 0;
        return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, dst, size, &got, nullptr) && got == size;
    };
    bool ok = false;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (read_at(0, &dos, sizeof dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 4096 &&
        read_at(static_cast<uint64_t>(dos.e_lfanew), &nt, sizeof nt) && nt.Signature == IMAGE_NT_SIGNATURE &&
        nt.FileHeader.NumberOfSections < 96) {
        const uint64_t sec_at = static_cast<uint64_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                nt.FileHeader.SizeOfOptionalHeader;
        for (WORD i = 0; i < nt.FileHeader.NumberOfSections && !ok; ++i) {
            IMAGE_SECTION_HEADER s{};
            if (!read_at(sec_at + i * sizeof s, &s, sizeof s)) break;
            if (rva >= s.VirtualAddress && rva + n <= s.VirtualAddress + s.SizeOfRawData) {
                out->resize(n);
                ok = read_at(static_cast<uint64_t>(s.PointerToRawData) + (rva - s.VirtualAddress), out->data(), n);
            }
        }
    }
    CloseHandle(f);
    if (!ok) *why = "the function's code is not in the exe file as expected";
    return ok;
}

// Executable memory within reach of the game's image (rel32 from the copy to the game's code and data).
inline uint8_t* alloc_near(uintptr_t lo, uintptr_t hi, size_t size) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t reach = 0x40000000;  // 1 GiB: the image is ~45 MB
    for (uintptr_t a = (lo - gran) & ~(gran - 1); a + reach > lo && a > gran; a -= gran) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m)) break;
        if (m.State != MEM_FREE) {
            a = reinterpret_cast<uintptr_t>(m.AllocationBase) & ~(gran - 1);
            continue;
        }
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
            return static_cast<uint8_t*>(p);
    }
    for (uintptr_t a = (hi + gran) & ~(gran - 1); a < hi + reach;) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m)) break;
        if (m.State == MEM_FREE)
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<uint8_t*>(p);
        const uintptr_t next = (reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize + gran - 1) & ~(gran - 1);
        if (next <= a) break;
        a = next;
    }
    return nullptr;
}

// A working copy of the function at `fn` (it must have an unwind entry of its own, without a handler or chain; at most
// 8 KiB), made from the exe file's bytes, placed within reach of the image, each of its near calls at a `sites` entry's
// `call_site` sent to that entry's `thunk` (through a 14-byte jump next to the copy), checked against the original
// (code_clone::same_meaning), with the original's unwind information registered for it. The copy is never freed. False
// (and `why`): nothing was made.
struct Copy {
    void* fn = nullptr;
    uint32_t size = 0;
    std::vector<uint8_t> file;  // the function's bytes as the exe file has them
};
struct Site {
    uintptr_t call_site = 0;
    const void* thunk = nullptr;
};
inline bool make(uintptr_t fn, const Site* sites, size_t count, Copy* out, std::string* why) {
    if (count == 0 || count > 4) return *why = "no call to send elsewhere", false;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(fn, &base, nullptr);
    if (!rf || base + rf->BeginAddress != fn) return *why = "the function has no unwind entry of its own", false;
    const uint32_t n = rf->EndAddress - rf->BeginAddress;
    if (n < 16 || n > 8192) return *why = "the function's size is not what a copy takes", false;
    for (size_t i = 0; i < count; ++i)
        if (sites[i].call_site <= fn || sites[i].call_site + 5 > fn + n)
            return *why = "the call to send elsewhere is not inside the function", false;
    if (rf->UnwindData & 1) return *why = "the function's unwind entry is chained", false;
    const auto* uw = reinterpret_cast<const uint8_t*>(base + rf->UnwindData);
    if ((uw[0] >> 3) != 0) return *why = "the function's unwind information has a handler or a chain", false;
    const uint32_t uw_len = 4 + 2 * ((uw[2] + 1u) & ~1u);
    std::vector<uint8_t> file;
    if (!read_file_code(static_cast<uint32_t>(fn - base), n, &file, why)) return false;
    // code, a 16-byte stub per thunk, the unwind information, the function table entry
    const uint32_t stub_at = (n + 15) & ~15u, uw_at = (stub_at + 16 * static_cast<uint32_t>(count) + 3) & ~3u,
                   rf_at = (uw_at + uw_len + 3) & ~3u;
    const uint32_t total = rf_at + sizeof(RUNTIME_FUNCTION);
    uint8_t* mem = alloc_near(static_cast<uintptr_t>(base), fn + n, total);
    if (!mem) return *why = "no memory within reach of the game's code", false;
    std::memset(mem, 0xCC, total);
    const auto at = reinterpret_cast<uint64_t>(mem);
    code_clone::Redirect redirects[4];
    for (size_t i = 0; i < count; ++i) {
        uint8_t* stub = mem + stub_at + 16 * i;  // jmp [rip+0]; dq thunk
        stub[0] = 0xFF, stub[1] = 0x25, stub[2] = stub[3] = stub[4] = stub[5] = 0;
        const uint64_t to = reinterpret_cast<uint64_t>(sites[i].thunk);
        std::memcpy(stub + 6, &to, 8);
        redirects[i] = {static_cast<uint32_t>(sites[i].call_site - fn), at + stub_at + 16 * i};
    }
    std::string err;
    if (!code_clone::relocate(file.data(), n, fn, mem, at, redirects, count, &err) ||
        !code_clone::same_meaning(file.data(), fn, mem, at, n, redirects, count, &err)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return *why = "the function could not be copied: " + err, false;
    }
    std::memcpy(mem + uw_at, uw, uw_len);
    RUNTIME_FUNCTION entry{};
    entry.BeginAddress = 0;
    entry.EndAddress = n;
    entry.UnwindData = uw_at;
    std::memcpy(mem + rf_at, &entry, sizeof entry);
    DWORD old = 0;
    if (!VirtualProtect(mem, total, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return *why = "the copy's memory cannot be made executable", false;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, total);
    if (!RtlAddFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(mem + rf_at), 1, at)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return *why = "the copy's unwind information was refused", false;
    }
    out->fn = mem;
    out->size = n;
    out->file = std::move(file);
    return true;
}

// One call sent elsewhere.
inline bool make(uintptr_t fn, uintptr_t call_site, const void* thunk, Copy* out, std::string* why) {
    const Site s{call_site, thunk};
    return make(fn, &s, 1, out, why);
}

}  // namespace code_copy
