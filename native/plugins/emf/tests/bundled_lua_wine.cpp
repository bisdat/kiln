// SPDX-License-Identifier: MIT
// Headless boot of the production kiln-emf plugin with fake core services. No game, world, or engine hooks.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include "kiln_api.h"
#include "services/kiln_profile.h"

namespace {
std::string game, kiln, logs, config, state, script;
std::string_view mode;
std::string boot_log;
std::mutex log_lock;
const char* smoke = "bundled-lua-smoke-ran";

std::string utf8(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

KilnStr kstr(std::string_view s) { return {s.data(), s.size()}; }

KilnProfileApi profile{
    .header = {sizeof(KilnProfileApi), KILN_PROFILE_VERSION},
    .mode = []() -> uint32_t {
        if (mode == "no-mods") return KILN_PROFILE_NO_MODS;
        if (mode == "invalid") return KILN_PROFILE_INVALID;
        return mode == "profile" ? KILN_PROFILE_ACTIVE : KILN_PROFILE_LEGACY;
    },
    .count = [](uint32_t kind) -> uint32_t { return mode == "profile" && kind == KILN_PROFILE_KIND_EMF ? 1 : 0; },
    .get = [](uint32_t kind, uint32_t index, KilnProfileMod* row) -> KilnResult {
        if (kind != KILN_PROFILE_KIND_EMF || index != 0 || mode != "profile") return KILN_ERR_NOT_FOUND;
        *row = {sizeof(KilnProfileMod), kind, kstr("lua-smoke"), kstr("1.0.0"), kstr(script), 1, 0};
        return KILN_OK;
    },
    .dropins = []() -> uint32_t { return mode == "legacy" ? 1u << KILN_PROFILE_KIND_EMF : 0; },
    .config_dir = []() { return kstr(config); },
};

int fail(const char* why) {
    std::fprintf(stderr, "EMF Lua bootstrap: %s (Windows error %lu)\n", why, GetLastError());
    return 1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return fail("usage: <legacy|profile|no-mods|invalid|dedicated|own-emf> <expected ignored|runs>");
    mode = argv[1];
    const std::string_view expect = argv[2];
    if (expect != "ignored" && expect != "runs") return fail("invalid expectation");
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n == MAX_PATH) return fail("test executable path unavailable");
    const auto root = std::filesystem::path(std::wstring(exe, n)).parent_path();
    if (mode == "own-emf" && !LoadLibraryW((root / L"emf_core.dll").c_str())) return fail("stand-in EMF loader did not load");
    game = utf8(root.wstring());
    kiln = game + "\\kiln";
    logs = kiln + "\\logs";
    config = kiln + "\\config";
    state = kiln + "\\state";
    script = game + "\\emf\\mods\\lua-smoke";

    KilnApi api{};
    api.size = sizeof api;
    api.abi_version = KILN_ABI_VERSION;
    api.side = []() -> uint32_t { return mode == "dedicated" ? KILN_SIDE_SERVER : KILN_SIDE_CLIENT; };
    api.log = [](KilnOwner, int32_t, KilnStr s) {
        std::lock_guard lock(log_lock);
        std::printf("%.*s\n", static_cast<int>(s.len), s.ptr);
        boot_log.append(s.ptr, s.len).push_back('\n');
    };
    api.path = [](int32_t kind, char* out, size_t cap, size_t* len) -> KilnResult {
        const std::string* p = nullptr;
        switch (kind) {
        case KILN_PATH_GAME: p = &game; break;
        case KILN_PATH_KILN: p = &kiln; break;
        case KILN_PATH_LOGS: p = &logs; break;
        case KILN_PATH_CONFIG: p = &config; break;
        case KILN_PATH_STATE: p = &state; break;
        default: return KILN_ERR_NOT_FOUND;
        }
        *len = p->size();
        if (*len > cap) return KILN_ERR_BUFFER_TOO_SMALL;
        std::memcpy(out, p->data(), *len);
        return KILN_OK;
    };
    api.owner_create = [](KilnStr) -> KilnOwner { static KilnOwner next = 1; return ++next; };
    api.owner_revoke = [](KilnOwner) {};
    api.build_info = [](KilnBuildInfo*) -> KilnResult { return KILN_ERR_NOT_FOUND; };
    api.feature = [](KilnStr) { return false; };
    api.service = [](KilnStr s, uint32_t) -> const void* {
        return std::string_view(s.ptr, s.len) == KILN_PROFILE_SERVICE ? &profile : nullptr;
    };
    api.symbol = [](KilnStr, uintptr_t*) -> KilnResult { return KILN_ERR_NOT_FOUND; };
    api.constant = [](KilnStr, int64_t*) -> KilnResult { return KILN_ERR_NOT_FOUND; };
    api.subscribe_tick = [](KilnOwner, int32_t, KilnTickFn, void*, KilnHandle*) -> KilnResult { return KILN_OK; };
    api.subscribe_world = [](KilnOwner, KilnWorldEventFn, void*, KilnHandle*) -> KilnResult { return KILN_OK; };
    api.subscribe_world_tick = [](KilnOwner, uint32_t, int32_t, KilnTickFn, void*, KilnHandle*) -> KilnResult { return KILN_OK; };
    api.server_world = []() -> uintptr_t { return 0; };
    api.world = [](uint32_t) -> uintptr_t { return 0; };

    const HMODULE plugin = LoadLibraryW((root / L"kiln/plugins/kiln-emf.dll").c_str());
    if (!plugin) return fail("kiln-emf did not load");
    const auto start = reinterpret_cast<KilnPluginMainFn>(GetProcAddress(plugin, "kiln_plugin_main"));
    KilnPluginInfo info{.size = sizeof(KilnPluginInfo)};
    if (!start || start(&api, 1, &info) != KILN_OK || !info.on_ready) return fail("kiln-emf did not register");
    info.on_ready(info.user);
    if (info.on_unload) info.on_unload(info.user);
    std::lock_guard lock(log_lock);
    const auto lua = GetModuleHandleW(L"lua.dll");
    if (expect == "ignored" && lua) return fail("unexpectedly mapped lua.dll");
    if (expect != "ignored" && !lua) return fail("lua.dll did not map");
    if ((expect == "runs") != (boot_log.find(smoke) != std::string::npos)) return fail("unexpected Lua script result");
    if (expect != "ignored" && boot_log.find("bundled): loaded") == std::string::npos) return fail("Lua did not load as a bundled mod");
    std::puts("PASS: bundled EMF Lua bootstrap");
    return 0;
}
