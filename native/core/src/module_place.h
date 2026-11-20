// SPDX-License-Identifier: MIT
// Which file a module is, for the lines about another module's hook (hooks_chain.cpp, compat_overlaps.cpp, sigdb.cpp):
// "dinput8.dll+0x3e080" alone cannot tell two DLLs of one name apart (a ShroudLink player addon and Building Tools, both
// a dinput8.dll from the Library, netjunk's report 2026-11-20). Pure string functions, tested on Linux too.
#pragma once
#include <string>
#include <string_view>

namespace kiln {

namespace module_place_detail {
inline char fold(char c) {
    if (c == '\\') return '/';
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}
inline std::string_view strip_long(std::string_view p) {
    if (p.substr(0, 4) == "\\\\?\\" || p.substr(0, 4) == "//?/") p.remove_prefix(4);
    return p;
}
}  // namespace module_place_detail

// A module's file as the state files and kiln.log name it: relative to the game folder with '/' separators
// ("kiln/chain/xhl-core/xhl-core.dll", "dinput8.dll" for one in the game folder itself), else its full path as given
// (a Library DLL: "...\Kiln\library\native\<mod id>\<version>\dinput8.dll", whose folder names the mod).
// A leading "\\?\" is dropped; the comparison ignores case and the separator kind. Empty for an empty path.
inline std::string module_path_text(std::string_view full, std::string_view game_dir) {
    using namespace module_place_detail;
    full = strip_long(full);
    game_dir = strip_long(game_dir);
    while (!game_dir.empty() && (game_dir.back() == '\\' || game_dir.back() == '/')) game_dir.remove_suffix(1);
    bool under = !game_dir.empty() && full.size() > game_dir.size() + 1 &&
                 (full[game_dir.size()] == '\\' || full[game_dir.size()] == '/');
    for (size_t i = 0; under && i < game_dir.size(); ++i) under = fold(full[i]) == fold(game_dir[i]);
    if (!under) return std::string(full);
    std::string rel(full.substr(game_dir.size() + 1));
    for (char& c : rel)
        if (c == '\\') c = '/';
    return rel;
}

// The bracket kiln.log puts after "<file>+0x<offset>": " [kiln/chain/xhl-core/xhl-core.dll]", " [game folder]" for a
// module in the game folder itself, " [<full path>]" outside it; empty without a path.
inline std::string module_place_suffix(std::string_view path_text) {
    if (path_text.empty()) return {};
    if (path_text.find_first_of("/\\") == std::string_view::npos) return " [game folder]";
    return " [" + std::string(path_text) + "]";
}

// True when chain entry `entry` (a path as chain.ini or the proxy name it) is the module whose path text
// (module_path_text) is `path_text`: the same full path, or for a game-relative text, an entry ending in "/<text>".
inline bool module_path_matches(std::string_view entry, std::string_view path_text) {
    using namespace module_place_detail;
    entry = strip_long(entry);
    if (path_text.empty() || entry.size() < path_text.size()) return false;
    const size_t at = entry.size() - path_text.size();
    for (size_t i = 0; i < path_text.size(); ++i)
        if (fold(entry[at + i]) != fold(path_text[i])) return false;
    return at == 0 || entry[at - 1] == '\\' || entry[at - 1] == '/';
}

}  // namespace kiln
