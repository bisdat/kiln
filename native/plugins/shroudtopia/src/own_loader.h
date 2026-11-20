// SPDX-License-Identifier: MIT
// Shared preflight so the core never waits for an external Shroudtopia loader.
#pragma once
#include "../../compat/compat.h"
#include <psapi.h>

namespace kshroud {
inline std::string own_loader_in_process(const std::filesystem::path& game) {
    HMODULE modules[2048]{};
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed)) return {};
    const auto count = std::min<size_t>(needed / sizeof(HMODULE), std::size(modules));
    for (size_t i = 0; i < count; ++i) {
        if (modules[i] == GetModuleHandleW(nullptr)) continue;
        wchar_t p[32768]{};
        const auto n = GetModuleFileNameW(modules[i], p, static_cast<DWORD>(std::size(p)));
        if (!n || n == std::size(p)) continue;
        const std::filesystem::path file(p);
        // Wine's builtin winmm.dll reports a game-folder winmm.dll's path without running that file.
        if (!kcompat::module_is_file(modules[i], file)) continue;
        if (kcompat::kiln_own_module(modules[i], file)) continue;  // Kiln's proxy and plugins name every engine
        if (kcompat::lower(kcompat::path_utf8(file.filename())) == "shroudtopia.dll" &&
            !kcompat::pe_exports_name(file, "CreateModInstance"))
            return "Shroudtopia's loader is loaded (" + kcompat::to_utf8(p) + ")";
        std::error_code ec;
        const bool in_game = std::filesystem::equivalent(file.parent_path(), game, ec);
        const bool chain = std::filesystem::equivalent(file.parent_path(), game / L"kiln" / L"chain", ec);
        if (!in_game && !chain) continue;
        // Exclude our own plugin and mods with the API strings; a loader is a process-level proxy, not a
        // CreateModInstance-exporting mod. Chain DLLs may be renamed, so inspect every eligible module path.
        if (modules[i] == GetModuleHandleW(L"kiln-shroudtopia.dll") ||
            kcompat::pe_exports_name(file, KILN_PLUGIN_MAIN) || kcompat::pe_exports_name(file, "CreateModInstance")) continue;
        std::string d;
        kcompat::read_file(p, &d);
        const std::string l = kcompat::lower(d);
        if (l.find("shroudtopia") != std::string::npos || l.find("shroudweave") != std::string::npos)
            return "a Shroudtopia loader proxy is loaded (" + kcompat::to_utf8(p) + ")";
    }
    return {};
}

}  // namespace kshroud
