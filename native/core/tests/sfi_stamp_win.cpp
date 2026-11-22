// SPDX-License-Identifier: MIT
// Windows check of a staged runtime (tools/package/stage.sh's folder), natively or under headless Wine
// (tests/test-sfi-stamp.sh): the core's own build-stamp test against the stage's kiln-sfi.dll. The core makes
// <game>/kiln/plugins/kiln-sfi.dll the ShroudForge-dialect owner only when its kiln_build_stamp() is the core's build
// identity (api.cpp load_one: launch::designate(owner, service_stamp_trusted(dll_build_stamp(h)), why)); without that,
// Kiln's interpreter never starts. Built with the core it checks (the same native build: the same kiln_version.h and
// kiln_build.h), over api.cpp's real "service lookup" and "service stamp" blocks (extracted by CMake at configure time):
// - this build has a dated build stamp, and the staged kiln-core.dll holds its build metadata (it is this build's core);
// - the staged kiln-sfi.dll, loaded as load_one loads a plugin, is trusted: the core designates it;
// - every native service DLL of the stage is trusted too (the stamp tools/sfi/sfi_stage.py compares kiln-sfi.dll with);
// - a copy of kiln-sfi.dll with another build's stamp, and a module without the export, are not trusted.
//   sfi-stamp-check <stage dir> <scratch dir>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "kiln_api.h"
#include "kiln_build.h"
#include "kiln_version.h"
#include "services/kiln_caps.h"
#include "services/kiln_hooks.h"
#include "services/kiln_launch.h"
#include "services/kiln_profile.h"
#include "services/kiln_progress.h"
#include "../src/tasks_host.h"              // the lookup block names the core's managed-host table (kiln.tasks)
#include "../src/hooks_protect_internal.h"  // and the core's kiln.internal.hooks table
#include "../src/owners_internal.h"         // and the core's kiln.internal.owners table
#include "../src/launch_internal.h"         // and the core's kiln.launch tables
#include "../../plugins/compat/original_code_service.h"  // and the core's original-code table

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += !ok;
}
}  // namespace

// What the core's blocks reach outside themselves, as stand-ins (none of it is part of this check). The build identity
// itself is the block's own build_stamp() over this build's kiln_version.h and kiln_build.h.
namespace kiln {
inline std::string_view sv(KilnStr s) { return s.ptr ? std::string_view(s.ptr, s.len) : std::string_view(); }
bool g_hooks = true;
namespace reg {
bool owner_alive(KilnOwner) { return true; }
std::string owner_name(KilnOwner o) { return "owner:" + std::to_string(o); }
const KilnOwnersInternalApi* owners_internal_table() { return nullptr; }
}  // namespace reg
namespace svc {
const void* find(std::string_view, uint32_t) { return nullptr; }
}
namespace profile { const void* table() { return nullptr; } }
namespace progress { const void* table() { return nullptr; } }
namespace tasks_svc { const void* host_table() { return nullptr; } }
namespace compat { const OriginalCodeApi* original_code_table() { return nullptr; } }
namespace launch {
const KilnLaunchApi* table() { return nullptr; }
const KilnLaunchInternalApi* internal_table() { return nullptr; }
}  // namespace launch
namespace hooks { const KilnHooksInternalApi* internal_table() { return nullptr; } }
namespace caps {
const void* table() { return nullptr; }
void service_published(KilnOwner, std::string_view, uint32_t) {}
}  // namespace caps
namespace guard {
bool task(KilnTaskFn fn, void* user, unsigned long* code) {
    __try {
        fn(user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}
bool copy(void* dst, uintptr_t src, size_t n) {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace guard
}  // namespace kiln
#define KLOG_I(...) (void)0
#define KLOG_W(...) (void)0
#define KLOG_E(...) (void)0

namespace kiln {
namespace {
#include "service_lookup.inc"
#include "service_stamp.inc"
}  // namespace
}  // namespace kiln

namespace {
std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

std::string read_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

size_t count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}

// The stamp of a module mapped as load_one maps a DLL (flags) or as an image only (DONT_RESOLVE_DLL_REFERENCES: no
// DllMain, no imports), read through the core's own reader.
std::string stamp_of(const std::wstring& path, DWORD flags, bool* loaded, DWORD* error = nullptr) {
    const HMODULE h = LoadLibraryExW(path.c_str(), nullptr, flags);
    *loaded = h != nullptr;
    if (error) *error = h ? 0 : GetLastError();
    if (!h) return {};
    const std::string stamp = kiln::dll_build_stamp(h);
    FreeLibrary(h);
    return stamp;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace kiln;
    if (argc != 3) {
        std::printf("usage: sfi-stamp-check <stage dir> <scratch dir>\n");
        return 2;
    }
    const std::wstring stage = argv[1], scratch = argv[2];
    const std::string core = build_stamp();
    const std::string meta = KILN_BUILD;
    check(!meta.empty() && core == std::string(KILN_VERSION) + meta, "this build has a dated build stamp: " + core);

    // the staged core is this build's: its own KILN_BUILD (boot.cpp's first log line, api.cpp's identity) is in it
    const std::string core_dll = read_file(stage + L"\\core\\kiln\\kiln-core.dll");
    check(core_dll.size() > 2 && core_dll.compare(0, 2, "MZ") == 0 && !meta.empty() &&
              count(core_dll, meta + std::string(1, '\0')) >= 1,
          "the staged kiln-core.dll holds this build's metadata " + meta);

    // kiln-sfi.dll, loaded as load_one loads kiln/plugins/*.dll: the stamp test of its designation
    const std::wstring sfi = stage + L"\\sfi\\kiln\\plugins\\kiln-sfi.dll";
    bool loaded = false;
    DWORD error = 0;
    const std::string sfi_stamp =
        stamp_of(sfi, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, &loaded, &error);
    check(loaded, "the staged kiln-sfi.dll loads as load_one loads it (LoadLibraryExW error " + std::to_string(error) + ")");
    check(service_stamp_trusted(sfi_stamp),
          "kiln-sfi.dll's stamp '" + sfi_stamp + "' is this core's '" + core + "': the core designates it");

    // every native service DLL of the stage carries the same identity
    int services = 0, trusted = 0;
    WIN32_FIND_DATAW found{};
    const std::wstring dir = stage + L"\\core\\kiln\\services";
    const HANDLE find = FindFirstFileW((dir + L"\\*.dll").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            bool mapped = false;
            const std::string s = stamp_of(dir + L"\\" + found.cFileName, DONT_RESOLVE_DLL_REFERENCES, &mapped);
            ++services;
            if (mapped && service_stamp_trusted(s)) ++trusted;
            else std::printf("     %s: stamp '%s'\n", narrow(found.cFileName).c_str(), s.c_str());
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    check(services > 0 && trusted == services,
          std::to_string(trusted) + " of " + std::to_string(services) + " staged service DLLs carry this core's stamp");

    // another build's kiln-sfi.dll (its stamp's last character changed, same length) and a module without the export
    std::string bytes = read_file(sfi);
    const std::string needle = core + std::string(1, '\0');
    const size_t at = bytes.find(needle);
    check(!core.empty() && count(bytes, needle) == 1, "kiln-sfi.dll holds its stamp once (" +
                                                          std::to_string(core.empty() ? 0 : count(bytes, needle)) + ")");
    if (at != std::string::npos) {
        char& last = bytes[at + core.size() - 1];
        last = last == '0' ? '1' : '0';
        const std::wstring stale = scratch + L"\\kiln-sfi-stale.dll";
        std::ofstream(stale, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bool mapped = false;
        const std::string s = stamp_of(stale, DONT_RESOLVE_DLL_REFERENCES, &mapped);
        check(mapped && !s.empty() && s != core && !service_stamp_trusted(s), "a stale kiln-sfi.dll is not trusted: " + s);
        DeleteFileW(stale.c_str());
    }
    check(!service_stamp_trusted(dll_build_stamp(GetModuleHandleW(nullptr))), "a module without kiln_build_stamp is not trusted");
    std::printf("sfi stamp check: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
