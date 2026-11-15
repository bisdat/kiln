// SPDX-License-Identifier: MIT
// Did the loader map the file it was asked for? Under Wine and Proton, LoadLibrary of a full path whose base name Wine
// also ships as a builtin (the real case: Ultimate ASI Loader as xinput9_1_0.dll next to the game) can map Wine's
// builtin in place of the file, and the module still reports the path that was asked for (GetModuleFileNameW and
// GetMappedFileNameW both do, Wine 11.0 and 11.20): only the image itself tells. So the loaded module's headers are
// compared with the file's, and Wine's builtin marker is looked for in the module when the file does not carry it.
//
// Shared by the proxy (dinput8.cpp: no CRT calls, no allocation, under the loader lock) and kiln-core's chain.ini [late]
// phase (native_mods.cpp), so both decide and word the warning alike: plain integer reads, no library calls, no
// Windows types (module_headers at the end is the one Windows part).
#pragma once
#include <cstddef>
#include <cstdint>

namespace kiln::image_match {

// The header fields a loader keeps as the file has them (relocation rewrites ImageBase, so that one is not compared).
struct Id {
    bool ok = false;            // DOS and PE32+ headers within the bytes
    bool wine_builtin = false;  // "Wine builtin DLL" right after the DOS header (offset 0x40), as winebuild writes it
    uint32_t time_date_stamp = 0, size_of_image = 0, check_sum = 0, entry_point = 0;
    uint16_t sections = 0;
};

namespace detail {
inline uint16_t u16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t u32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
constexpr char kWineMarker[] = "Wine builtin DLL";
}  // namespace detail

// The first n bytes of a file, or of a loaded module's header page.
inline Id read(const unsigned char* b, size_t n) {
    Id id;
    if (!b || n < 0x40 || b[0] != 'M' || b[1] != 'Z') return id;
    const uint32_t at = detail::u32(b + 0x3c);
    // "PE\0\0" (4), IMAGE_FILE_HEADER (20), IMAGE_OPTIONAL_HEADER64 up to and including CheckSum (68)
    if (at > n || n - at < 4 + 20 + 68) return id;
    const unsigned char* nt = b + at;
    const unsigned char* opt = nt + 24;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] || nt[3] || detail::u16(opt) != 0x20b) return id;
    id.sections = detail::u16(nt + 6);
    id.time_date_stamp = detail::u32(nt + 8);
    id.entry_point = detail::u32(opt + 16);
    id.size_of_image = detail::u32(opt + 56);
    id.check_sum = detail::u32(opt + 64);
    constexpr size_t m = sizeof detail::kWineMarker - 1;
    if (n >= 0x40 + m) {
        id.wine_builtin = true;
        for (size_t i = 0; i < m && id.wine_builtin; ++i) id.wine_builtin = b[0x40 + i] == detail::kWineMarker[i];
    }
    id.ok = true;
    return id;
}

// Signs that the module is not the file: kFieldsDiffer (a kept header field differs), kWineBuiltin (the module is
// Wine's builtin and the file is not). 0 when they match, and when either side could not be read: an unreadable
// file or a module that hid its headers is no proof of anything.
enum : unsigned { kFieldsDiffer = 1u, kWineBuiltin = 2u };
inline unsigned mismatch(const Id& loaded, const Id& file) {
    if (!loaded.ok || !file.ok) return 0;
    unsigned signs = 0;
    if (loaded.time_date_stamp != file.time_date_stamp || loaded.size_of_image != file.size_of_image ||
        loaded.check_sum != file.check_sum || loaded.entry_point != file.entry_point || loaded.sections != file.sections)
        signs |= kFieldsDiffer;
    if (loaded.wine_builtin && !file.wine_builtin) signs |= kWineBuiltin;
    return signs;
}

// What the warning names in the asked file's place: the path the module reports when that is another file, else
// "Wine's own <name>" when the module is Wine's builtin (Wine reports the asked path for it), else nothing.
enum class Instead { unknown, reported_path, wine_own };
inline Instead instead(bool reported_is_another_file, const Id& loaded) {
    if (reported_is_another_file) return Instead::reported_path;
    return loaded.wine_builtin ? Instead::wine_own : Instead::unknown;
}

// The warning line (no line end) into out[cap], NUL-terminated; false when it does not fit (out is then empty):
//   <prefix>requested <asked> but another file was loaded in its place[: <reported> | : Wine's own <name>]; on Wine
//   or Proton a DLL override is needed: WINEDLLOVERRIDES="<name>=n,b"
// <name>: the asked file's base name without ".dll". C is char (UTF-8) or wchar_t.
template <class C>
bool warning(C* out, size_t cap, const C* prefix, const C* asked, Instead what, const C* reported) {
    size_t k = 0;
    bool fits = cap > 0;
    auto put = [&](C c) {
        if (k + 1 < cap) out[k++] = c;
        else fits = false;
    };
    auto lit = [&](const char* s) {  // ASCII text of this header
        while (*s) put(static_cast<C>(*s++));
    };
    auto str = [&](const C* s, size_t n) {
        for (size_t i = 0; s && i < n && s[i]; ++i) put(s[i]);
    };
    const C* base = asked;
    size_t len = 0;
    for (const C* p = asked; p && *p; ++p, ++len)
        if (*p == C('\\') || *p == C('/')) base = p + 1;
    size_t name = static_cast<size_t>(asked + len - base);
    if (name > 4) {
        const C* e = base + name - 4;
        auto lower = [](C c) { return c >= C('A') && c <= C('Z') ? static_cast<C>(c - C('A') + C('a')) : c; };
        if (e[0] == C('.') && lower(e[1]) == C('d') && lower(e[2]) == C('l') && lower(e[3]) == C('l')) name -= 4;
    }
    str(prefix, static_cast<size_t>(-1));
    lit("requested ");
    str(asked, len);
    lit(" but another file was loaded in its place");
    if (what == Instead::reported_path) {
        lit(": ");
        str(reported, static_cast<size_t>(-1));
    } else if (what == Instead::wine_own) {
        lit(": Wine's own ");
        str(base, name);
    }
    lit("; on Wine or Proton a DLL override is needed: WINEDLLOVERRIDES=\"");
    str(base, name);
    lit("=n,b\"");
    if (cap) out[fits ? k : 0] = 0;
    return fits;
}

}  // namespace kiln::image_match

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace kiln::image_match {

// The loaded module's header page as an Id: not ok when the page cannot be read (a DLL may protect or wipe its own
// headers in its DllMain; that is no proof of a swap).
inline Id module_headers(HMODULE h) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!h || VirtualQuery(h, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT || mbi.BaseAddress != h ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)))
        return Id{};
    return read(reinterpret_cast<const unsigned char*>(h), mbi.RegionSize < 4096 ? mbi.RegionSize : 4096);
}

}  // namespace kiln::image_match
#endif
