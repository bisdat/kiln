// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <cstdint>
namespace kiln::saves {
// Only an absent directory entry in a known slot folder proves a new world. Corrupt/unreadable headers,
// unknown slots and dangling links do not. Called after the game's load reports success.
inline bool new_world_file(const std::filesystem::path& path, bool header_read, bool identity_read = true,
                           uint32_t revision = 0, bool has_guid = false) {
    if (header_read || path.empty() || !identity_read || revision != 0 || has_guid) return false;
    std::error_code ec;
    if (!std::filesystem::is_directory(path.parent_path(), ec) || ec) return false;
    const auto st = std::filesystem::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return false;
    return st.type() == std::filesystem::file_type::not_found;
}
}
