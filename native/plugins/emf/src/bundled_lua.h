// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>

namespace kemf::bundled_lua {

inline constexpr std::string_view id = "lua";
inline constexpr wchar_t folder[] = L"emf-lua";  // under kiln/plugins (component emf)

constexpr bool is_lua_main(std::string_view main) {
    if (main.size() <= 4) return false;
    const auto ext = main.substr(main.size() - 4);
    return ext[0] == '.' && (ext[1] == 'l' || ext[1] == 'L') && (ext[2] == 'u' || ext[2] == 'U') &&
           (ext[3] == 'a' || ext[3] == 'A');
}

// Observe the mods that won discovery, including disabled/rejected explicit hosts. The existing switches and
// dependency resolver still decide whether this host and its scripts actually run.
struct Demand {
    bool explicit_host = false;
    bool script = false;

    constexpr void consider(std::string_view mod_id, std::string_view profile_id, std::string_view main,
                            bool enabled_pending, uint32_t api_min, uint32_t api_version,
                            bool dedicated, std::string_view side) {
        explicit_host |= mod_id == id || profile_id == id;
        script |= enabled_pending && api_min <= api_version && (!dedicated || side != "client") && is_lua_main(main);
    }
    constexpr bool needed() const { return script && !explicit_host; }
};

}  // namespace kemf::bundled_lua
