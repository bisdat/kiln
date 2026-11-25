// SPDX-License-Identifier: MIT
#include "bundled_lua.h"

using kemf::bundled_lua::Demand;

constexpr bool required(bool enabled, bool dedicated, std::string_view side, uint32_t api, std::string_view main) {
    Demand d;
    d.consider("script", "script", main, enabled, api, 49, dedicated, side);
    return d.needed();
}

constexpr bool explicit_wins(std::string_view id, std::string_view profile_id, bool enabled) {
    Demand d;
    d.consider("script", "", "main.lua", true, 14, 49, false, "server");
    d.consider(id, profile_id, "", enabled, 20, 49, false, "");
    return !d.needed();
}

static_assert(!Demand{}.needed());
static_assert(required(true, false, "server", 14, "main.lua"));  // hosted game
static_assert(required(true, true, "server", 49, "main.LuA"));
static_assert(required(true, false, "client", 20, "scripts/main.lua"));
static_assert(!required(false, false, "server", 14, "main.lua"));
static_assert(!required(true, true, "client", 14, "main.lua"));
static_assert(!required(true, false, "server", 50, "main.lua"));
static_assert(!required(true, false, "server", 14, ""));
static_assert(!required(true, false, "server", 14, "main.js"));
static_assert(!required(true, false, "server", 14, "main.lua.bak"));
static_assert(explicit_wins("lua", "lua", true));
static_assert(explicit_wins("lua", "", false));  // disabled or rejected drop-in: do not substitute
static_assert(explicit_wins("other", "lua", false));  // selected profile entry has a mismatched manifest

int main() {}
