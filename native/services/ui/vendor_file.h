// SPDX-License-Identifier: MIT
// kiln.ui: the vendor files of Kiln's upscalers (AMD FSR, Intel XeSS), checked before the game loads them
// (docs/kiln/graphics.md "Upscaler behind the game's FSR 3"; 0.2.0-dev.29.18 phase B).
//
// The launcher downloads them on the player's request into <game>/kiln/vendor/<vendor>/<version>/ and checks each
// against the size and SHA-256 Kiln pinned (launcher/src/data/vendor-components.json). The game checks again, here,
// with the same pins (vendor_pins.h, generated from that manifest), because anything may have written the folder since:
//  - only <game>/kiln/vendor/<vendor>/<version>/<name>, never the game's root or the search path;
//  - no link (a reparse point, which includes Wine's Unix links) on kiln\vendor, the vendor's folder, the version's
//    folder or the file;
//  - each file is opened with writes and deletes denied, hashed through that very handle, and the handle stays open for
//    the session: what loads is what was hashed (LoadLibraryExW maps the same file; nobody can change it meanwhile);
//  - a file the manifest has not pinned is never loaded.
// The logic is plain C++ over a small file interface (Fs), so the host test (tests/vendor_file_test.cpp) runs it on
// Linux files; the Windows interface and the loader are at the end, under _WIN32.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "sha256.h"
#include "vendor_pins.h"

namespace kui::vendor {

// The pins of one component (vendor-components.json id, "intel-xess"), in the manifest's order.
inline std::vector<const Pin*> component_pins(std::string_view component) {
    std::vector<const Pin*> out;
    for (const Pin& p : kPins)
        if (component == p.component) out.push_back(&p);
    return out;
}

// What the check of one component found. Stable order: the first problem found is the one reported.
enum class Check : uint32_t {
    ok = 0,
    no_pins,     // the manifest pins no file of this component (or names no such component)
    missing,     // a file is not there (not downloaded, or removed)
    link,        // a folder on the way or the file is a link
    unreadable,  // there, but could not be opened or read (in use by something that denies reading, no access)
    size,        // not the pinned size
    hash,        // not the pinned SHA-256
};
inline const char* check_name(Check c) {
    switch (c) {
        case Check::ok: return "ok";
        case Check::no_pins: return "no_pins";
        case Check::missing: return "missing";
        case Check::link: return "link";
        case Check::unreadable: return "unreadable";
        case Check::size: return "size";
        case Check::hash: return "hash";
    }
    return "?";
}
// Which upscale::Why a failed check is (upscale_backend.h): missing files -> files_missing (2), every other problem with
// the files -> files_bad (3), no pins -> unavailable (12). Kept as numbers so this header needs nothing of the backends.
inline uint32_t check_why(Check c) {
    switch (c) {
        case Check::ok: return 0;
        case Check::missing: return 2;
        case Check::no_pins: return 12;
        default: return 3;
    }
}

// The file system as the check uses it. Paths are UTF-16 (Windows); the host test maps them to its own files.
class Fs {
  public:
    using Handle = void*;
    virtual ~Fs() = default;
    virtual bool exists(const std::wstring& path) = 0;   // a file or folder (a link counts, whatever it points to)
    virtual bool is_link(const std::wstring& path) = 0;  // a reparse point / symbolic link (not followed)
    // Opened for reading, writes and deletes by anybody else denied, links not followed. nullptr: could not.
    virtual Handle open_locked(const std::wstring& path, uint32_t* error) = 0;
    virtual bool handle_is_link(Handle h) = 0;           // the opened object itself is a link
    virtual bool size(Handle h, uint64_t* out) = 0;
    virtual bool read(Handle h, void* buf, uint32_t want, uint32_t* got) = 0;  // sequential, from the start
    virtual void close(Handle h) = 0;
};

// One component, checked. On success the handles stay open (writes denied) until release(): keep them for the session.
struct Verified {
    Check check = Check::no_pins;
    std::string detail;               // English, for kiln.log and the status ("libxess.dll is not the file Kiln pinned ...")
    std::string component, vendor, version;
    std::wstring dir;                 // <kiln>\vendor\<vendor>\<version>
    std::vector<std::wstring> paths;  // full paths, the manifest's order
    std::vector<Fs::Handle> handles;  // open, one per path (ok only)
    bool ok() const { return check == Check::ok; }
};

inline std::wstring widen(std::string_view s) {  // the pins and Kiln's paths: ASCII names; UTF-8 decoded for the rest
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = static_cast<uint8_t>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (n > 1 && i + n <= s.size()) {
            c &= (0xff >> (n + 1));
            for (size_t k = 1; k < n; ++k) c = (c << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3f);
        } else {
            n = 1;
        }
        i += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xd800 + (c >> 10)));
            out.push_back(static_cast<wchar_t>(0xdc00 + (c & 0x3ff)));
        } else {
            out.push_back(static_cast<wchar_t>(c));
        }
    }
    return out;
}
inline std::string narrow(const std::wstring& w) {  // for log lines: UTF-16 to UTF-8
    std::string out;
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t c = static_cast<uint16_t>(w[i]);
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < w.size()) c = 0x10000 + ((c - 0xd800) << 10) + (static_cast<uint16_t>(w[++i]) - 0xdc00);
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) out += {static_cast<char>(0xc0 | (c >> 6)), static_cast<char>(0x80 | (c & 0x3f))};
        else if (c < 0x10000)
            out += {static_cast<char>(0xe0 | (c >> 12)), static_cast<char>(0x80 | ((c >> 6) & 0x3f)), static_cast<char>(0x80 | (c & 0x3f))};
        else
            out += {static_cast<char>(0xf0 | (c >> 18)), static_cast<char>(0x80 | ((c >> 12) & 0x3f)),
                    static_cast<char>(0x80 | ((c >> 6) & 0x3f)), static_cast<char>(0x80 | (c & 0x3f))};
    }
    return out;
}

// The SHA-256 of an open file, read to its end in 1 MB pieces. False: a read failed.
inline bool hash_handle(Fs& fs, Fs::Handle h, std::string* hex, uint64_t* bytes) {
    Sha256 sha;
    std::vector<uint8_t> buf(1u << 20);
    uint64_t total = 0;
    for (;;) {
        uint32_t got = 0;
        if (!fs.read(h, buf.data(), static_cast<uint32_t>(buf.size()), &got)) return false;
        if (!got) break;
        sha.update(buf.data(), got);
        total += got;
    }
    *hex = sha.hex();
    *bytes = total;
    return true;
}

// Releases a Verified's handles (the files may then change; a loaded DLL stays mapped as it was).
inline void release(Fs& fs, Verified* v) {
    for (auto h : v->handles) fs.close(h);
    v->handles.clear();
}

// Checks every file of `component` in <kiln_dir>\vendor\<vendor>\<version>\ against its pin. `kiln_dir`: <game>\kiln
// (KILN_PATH_KILN), no trailing separator. On failure every handle opened is closed again and nothing is kept.
inline Verified verify_component(Fs& fs, const std::wstring& kiln_dir, std::string_view component) {
    Verified v;
    v.component = std::string(component);
    const auto pins = component_pins(component);
    if (pins.empty()) {
        v.check = Check::no_pins;
        v.detail = "Kiln pins no vendor file of " + v.component + " in this build";
        return v;
    }
    v.vendor = pins[0]->vendor;
    v.version = pins[0]->version;
    const std::wstring vendor_dir = kiln_dir + L"\\vendor";
    const std::wstring dir = vendor_dir + L"\\" + widen(v.vendor);
    v.dir = dir + L"\\" + widen(v.version);
    const std::string where = "kiln/vendor/" + v.vendor + "/" + v.version;
    auto fail = [&](Check c, std::string detail) {
        release(fs, &v);
        v.paths.clear();
        v.check = c;
        v.detail = std::move(detail);
        return v;
    };
    // the folders first: a link on the way could point anywhere
    const std::wstring folders[] = {vendor_dir, dir, v.dir};
    const char* names[] = {"kiln/vendor", "kiln/vendor/", ""};
    for (int i = 0; i < 3; ++i) {
        if (!fs.exists(folders[i])) {
            return fail(Check::missing, where + " is not there: download " + v.component + " in the launcher (Settings, Upscalers)");
        }
        if (fs.is_link(folders[i])) {
            const std::string n = i == 0 ? names[0] : i == 1 ? std::string(names[1]) + v.vendor : where;
            return fail(Check::link, n + " is a link: Kiln loads vendor files only from its own folder (download " + v.component + " again)");
        }
    }
    for (const Pin* p : pins) {
        const std::wstring path = v.dir + L"\\" + widen(p->name);
        const std::string label = where + "/" + p->name;
        if (!fs.exists(path)) return fail(Check::missing, label + " is missing: download " + v.component + " again in the launcher");
        if (fs.is_link(path)) return fail(Check::link, label + " is a link: Kiln loads only the file itself (download " + v.component + " again)");
        uint32_t err = 0;
        Fs::Handle h = fs.open_locked(path, &err);
        if (!h) {
            char b[64];
            std::snprintf(b, sizeof b, " (error %u)", err);
            return fail(Check::unreadable, label + " could not be opened for the check" + b + ": something else may hold it open for writing");
        }
        v.handles.push_back(h);
        v.paths.push_back(path);
        if (fs.handle_is_link(h)) return fail(Check::link, label + " is a link: Kiln loads only the file itself (download " + v.component + " again)");
        uint64_t size = 0;
        if (!fs.size(h, &size)) return fail(Check::unreadable, label + " could not be read for the check");
        if (size != p->size) {
            return fail(Check::size, label + " has " + std::to_string(size) + " bytes, not the " + std::to_string(p->size) +
                                         " of the file Kiln pinned: download " + v.component + " again");
        }
        std::string hex;
        uint64_t read = 0;
        if (!hash_handle(fs, h, &hex, &read) || read != size) return fail(Check::unreadable, label + " could not be read to its end for the check");
        if (hex != p->sha256) {
            return fail(Check::hash, label + " is not the file Kiln pinned (its SHA-256 is " + hex.substr(0, 16) +
                                         "..., not " + std::string(p->sha256).substr(0, 16) + "...): download " + v.component + " again");
        }
    }
    v.check = Check::ok;
    v.detail = where + ": " + std::to_string(pins.size()) + " file(s), each the one Kiln pinned";
    return v;
}

}  // namespace kui::vendor

#ifdef _WIN32
#include <windows.h>

namespace kui::vendor {

// The real file system. Unix links under Wine show as reparse points.
class WinFs final : public Fs {
  public:
    bool exists(const std::wstring& path) override { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
    bool is_link(const std::wstring& path) override {
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
    }
    Handle open_locked(const std::wstring& path, uint32_t* error) override {
        // Others may read (the loader maps the file through its own handle) but neither write, rename nor delete it.
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (error) *error = GetLastError();
            return nullptr;
        }
        return h;
    }
    bool handle_is_link(Handle h) override {
        BY_HANDLE_FILE_INFORMATION i{};
        return !GetFileInformationByHandle(static_cast<HANDLE>(h), &i) || (i.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
               (i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    }
    bool size(Handle h, uint64_t* out) override {
        LARGE_INTEGER s{};
        if (!GetFileSizeEx(static_cast<HANDLE>(h), &s) || s.QuadPart < 0) return false;
        *out = static_cast<uint64_t>(s.QuadPart);
        return true;
    }
    bool read(Handle h, void* buf, uint32_t want, uint32_t* got) override {
        DWORD n = 0;
        const BOOL ok = ReadFile(static_cast<HANDLE>(h), buf, want, &n, nullptr);
        *got = n;
        return ok != FALSE;
    }
    void close(Handle h) override { CloseHandle(static_cast<HANDLE>(h)); }
};

// Loads one verified file (a path of Verified::paths, while its handle is open). Its own folder is searched for the
// libraries it imports, then the system's (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | DEFAULT_DIRS); never the game's root
// first or the PATH. Checks that the module the loader returns was mapped from that path (not some other module of
// the same name already in the process). nullptr: `detail` says why (English).
inline HMODULE load_verified(const std::wstring& path, std::string* detail) {
    const std::string name = narrow(path.substr(path.find_last_of(L"\\/") + 1));
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) {
        const DWORD e = GetLastError();
        char b[96];
        std::snprintf(b, sizeof b, " (Windows error %lu", static_cast<unsigned long>(e));
        std::string why = name + " did not load" + b;
        if (e == ERROR_MOD_NOT_FOUND) why += ": a library it needs is missing";
        else if (e == ERROR_BAD_EXE_FORMAT) why += ": not a 64-bit Windows library";
        else if (e == ERROR_DLL_INIT_FAILED) why += ": its start-up failed";
        else if (e == ERROR_PROC_NOT_FOUND) why += ": a library it needs is too old";
        why += ")";
        if (detail) *detail = why;
        return nullptr;
    }
    wchar_t got[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(m, got, static_cast<DWORD>(sizeof got / sizeof got[0]));
    wchar_t want[MAX_PATH * 2];
    const DWORD w = GetFullPathNameW(path.c_str(), static_cast<DWORD>(sizeof want / sizeof want[0]), want, nullptr);
    const bool same = n && w && n < sizeof got / sizeof got[0] && w < sizeof want / sizeof want[0] &&
                      CompareStringOrdinal(got, static_cast<int>(n), want, static_cast<int>(w), TRUE) == CSTR_EQUAL;
    if (!same) {
        // Not ours: leave the other module alone (this reference goes again) and say so.
        FreeLibrary(m);
        if (detail) *detail = name + " is already loaded in the game from " + (n ? narrow(std::wstring(got, n)) : std::string("elsewhere")) +
                              ", not from Kiln's folder: Kiln does not use it";
        return nullptr;
    }
    return m;
}

}  // namespace kui::vendor
#endif
