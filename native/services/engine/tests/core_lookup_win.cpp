// SPDX-License-Identifier: MIT
// Release-build check under headless Wine (tests/wine.sh): the core's real service lookup/registration block and its
// real build-stamp reader (native/core/src/api.cpp, between their markers; extracted by CMake at configure time) against
// the DLLs this build produced:
// - every DLL in bin/services and bin/plugins (kiln_service/kiln_plugin outputs) carries this build's stamp, read the
//   way load_one reads it (mapped without running DllMain);
// - the Release kiln-engine.dll boots through the services' KilnApi exactly as load_one boots a trusted service:
//   kiln.internal.engine resolves for services only, the consumer helper accepts it, plugins see nothing;
// - a copy of kiln-engine.dll with another build's stamp, one from a build without build metadata (version only), and
//   a module without the export, are not trusted;
// - kiln.internal.launch (the core's own) resolves for services only, and the Release kiln-engine.dll looks it up
//   exactly once, at init, through the services' KilnApi.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "include/kiln_engine_native.h"
#include "kiln_api.h"
#include "kiln_build.h"
#include "kiln_version.h"
#include "services/kiln_caps.h"
#include "services/kiln_engine.h"
#include "services/kiln_hooks.h"
#include "services/kiln_launch.h"
#include "services/kiln_profile.h"
#include "services/kiln_progress.h"
#include "../../../core/src/tasks_host.h"  // the lookup block names the core's managed-host table (kiln.tasks)
#include "../../../core/src/hooks_protect_internal.h"  // and the core's kiln.internal.hooks table
#include "../../../core/src/owners_internal.h"  // and the core's kiln.internal.owners table
#include "../../../core/src/launch_internal.h"  // and the core's kiln.launch tables
#include "../../../plugins/compat/original_code_service.h"  // and the core's original-code table

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += !ok;
}
void KILN_CALL log_line(KilnOwner, int32_t, KilnStr t) { std::printf("log: %.*s\n", static_cast<int>(t.len), t.ptr); }
KilnResult KILN_CALL build_info(KilnBuildInfo* out) {
    std::memset(out->exe_sha256, 'a', 64);
    out->exe_sha256[64] = 0;
    out->side = KILN_SIDE_CLIENT;
    return KILN_OK;
}
bool KILN_CALL on_world_thread(uint32_t) { return false; }
}  // namespace

// What the core's blocks reach outside themselves (registry, built-in tables, caps, fault guard), as small stand-ins.
// The build identity itself is the block's own build_stamp() over this build's kiln_version.h and kiln_build.h.
namespace kiln {
inline std::string_view sv(KilnStr s) { return s.ptr ? std::string_view(s.ptr, s.len) : std::string_view(); }
bool g_hooks = true;
std::set<KilnOwner> alive{1, 2};
namespace reg {
bool owner_alive(KilnOwner o) { return o == 0 || alive.count(o); }
std::string owner_name(KilnOwner o) { return "owner:" + std::to_string(o); }
const KilnOwnersInternalApi* owners_internal_table() { return nullptr; }  // kiln.internal.owners: not part of this check
}  // namespace reg
namespace svc {
const void* find(std::string_view, uint32_t) { return nullptr; }
}
namespace profile { const void* table() { return nullptr; } }
namespace progress { const void* table() { return nullptr; } }
namespace tasks_svc {
const void* host_table() { return nullptr; }  // kiln.tasks' managed-host table: not part of this check
}  // namespace tasks_svc
namespace compat {
const OriginalCodeApi* original_code_table() { return nullptr; }  // the core's original-code table: not part of this check
}  // namespace compat
namespace launch {
const KilnLaunchApi* table() { return nullptr; }  // kiln.launch: not part of this check
int internal_lookups = 0;                         // how often the block served kiln.internal.launch
KilnOwner KILN_CALL nobody() { return 0; }
const KilnLaunchInternalApi g_internal = {{sizeof(KilnLaunchInternalApi), KILN_LAUNCH_INTERNAL_VERSION}, &nobody};
const KilnLaunchInternalApi* internal_table() {
    ++internal_lookups;
    return &g_internal;
}
}  // namespace launch
namespace hooks {
const KilnHooksInternalApi* internal_table() { return nullptr; }  // kiln.internal.hooks: not part of this check
}  // namespace hooks
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

// Every DLL in <bin>\<folder>, mapped without DllMain or imports, read through the core's own stamp reader.
int stamps_in(const std::wstring& bin, const wchar_t* folder) {
    int count = 0;
    WIN32_FIND_DATAW found{};
    const std::wstring dir = bin + L"\\" + folder;
    const HANDLE find = FindFirstFileW((dir + L"\\*.dll").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::wstring path = dir + L"\\" + found.cFileName;
        const HMODULE h = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        const std::string stamp = h ? kiln::dll_build_stamp(h) : std::string();
        check(h && kiln::service_stamp_trusted(stamp),
              narrow(found.cFileName) + ": stamp '" + stamp + "' is this build's '" + kiln::build_stamp() + "'");
        if (h) FreeLibrary(h);
        ++count;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return count;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace kiln;
    if (argc < 5) {
        std::printf("usage: engine-core-lookup <bin dir> <stale copy> <unstamped copy> <kiln-engine.dll>\n");
        return 2;
    }
    const std::string core = build_stamp();
    check(core.size() > std::strlen(KILN_VERSION) + 10 && core.find('+') != std::string::npos,
          "this build has a dated build stamp: " + core);
    const int services = stamps_in(argv[1], L"services"), plugins = stamps_in(argv[1], L"plugins");
    check(services > 0 && plugins > 0, "stamps read from " + std::to_string(services) + " services and " +
                                           std::to_string(plugins) + " plugins");
    // a stale copy (another build's stamp) and a module without the export are never trusted
    const HMODULE stale = LoadLibraryExW(argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    const std::string stale_stamp = stale ? dll_build_stamp(stale) : std::string();
    check(stale && !stale_stamp.empty() && !service_stamp_trusted(stale_stamp), "a stale copy is not trusted: " + stale_stamp);
    check(!service_stamp_trusted(dll_build_stamp(GetModuleHandleW(nullptr))), "a module without kiln_build_stamp is not trusted");
    const HMODULE unstamped = LoadLibraryExW(argv[3], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    const std::string unstamped_stamp = unstamped ? dll_build_stamp(unstamped) : std::string();
    check(unstamped && unstamped_stamp == KILN_VERSION && !service_stamp_trusted(unstamped_stamp),
          "a DLL from a build without build metadata (version only) is not trusted: " + unstamped_stamp);

    // load_one's service path with the Release kiln-engine.dll
    const HMODULE engine = LoadLibraryExW(argv[4], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    check(engine != nullptr, "the Release kiln-engine.dll loads");
    if (!engine) return 1;
    const std::string stamp = dll_build_stamp(engine);
    check(service_stamp_trusted(stamp), "kiln-engine.dll is trusted: " + stamp);
    const KilnOwner owner = 1;
    service_owner_add(owner);
    KilnApi plugins_api{};
    plugins_api.size = sizeof plugins_api;
    plugins_api.abi_version = KILN_ABI_VERSION;
    plugins_api.service = &api_service;
    plugins_api.register_service = &api_register_service;
    plugins_api.log = &log_line;
    plugins_api.build_info = &build_info;
    plugins_api.on_world_thread = &on_world_thread;
    const KilnApi services_api = services_api_from(plugins_api);
    auto main_fn = reinterpret_cast<KilnPluginMainFn>(GetProcAddress(engine, "kiln_plugin_main"));
    KilnPluginInfo info{};
    info.size = sizeof info;
    check(main_fn && main_fn(&services_api, owner, &info) == KILN_OK, "kiln_plugin_main through the services' KilnApi");
    check(launch::internal_lookups == 1, "the Release kiln-engine.dll looked kiln.internal.launch up once, at init (" +
                                             std::to_string(launch::internal_lookups) + ")");
    const KilnStr internal{KILN_ENGINE_NATIVE_SERVICE, sizeof KILN_ENGINE_NATIVE_SERVICE - 1};
    const KilnStr open{KILN_ENGINE_SERVICE, sizeof KILN_ENGINE_SERVICE - 1};
    check(api_service_services(internal, 1) != nullptr && api_service(internal, 1) == nullptr && api_service(open, 1) != nullptr,
          "kiln.internal.engine resolves for services only; kiln.engine for everyone");
    const KilnStr launch_internal{KILN_LAUNCH_INTERNAL_SERVICE, sizeof KILN_LAUNCH_INTERNAL_SERVICE - 1};
    check(api_service_services(launch_internal, 1) != nullptr && api_service(launch_internal, 1) == nullptr &&
              api_service_services(launch_internal, KILN_LAUNCH_INTERNAL_VERSION + 1) == nullptr,
          "kiln.internal.launch resolves for services only, and not as a newer version");
    char why[200]{};
    const KilnEngineNativeApi* n = kiln_engine_native(&services_api, owner, sizeof(KilnEngineNativeApi), why, sizeof why);
    check(n && n->header.size == sizeof(KilnEngineNativeApi), std::string("the consumer helper accepts the Release table ") + why);
    check(!kiln_engine_native(&plugins_api, 2, sizeof(KilnEngineNativeApi), why, sizeof why), "a plugin's KilnApi sees nothing");
    const KilnServiceHeader table{sizeof(KilnServiceHeader), 1};
    check(plugins_api.register_service(owner, KilnStr{"kiln.internal.forged", 20}, &table) == KILN_ERR_INVALID_ARGUMENT,
          "a plugin's KilnApi publishes no internal table, even with a service owner");
    std::printf("engine core lookup (Release): %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
