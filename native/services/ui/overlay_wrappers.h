// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace kui::overlay_guard {
inline constexpr std::array<const wchar_t*, 6> wrapper_names = {
    L"sl.interposer.dll", L"nvngx_dlss.dll", L"dxgi.dll", L"ReShade64.dll", L"opengl32.dll", L"powrprof.dll"
};
inline constexpr std::array<bool, 6> system_names = {false, false, true, false, true, true};
using LoadedWrappers = std::array<std::filesystem::path, wrapper_names.size()>;
struct Wrappers { std::string identity; bool streamline = false; };
// Canonical paths deduplicate an installed DLL and its loaded copy. Windows paths are
// case insensitive; neither a different spelling nor load timing changes the identity.
inline std::string wrapper_path(const std::filesystem::path& file) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(file, ec);
    auto name = (ec ? file.lexically_normal() : canonical).generic_u8string();
    for (auto& c : name) if (c >= u8'A' && c <= u8'Z') c += u8'a' - u8'A';
    return std::string(name.begin(), name.end());
}
inline Wrappers detect_wrappers(const std::filesystem::path& game, const LoadedWrappers& loaded = {}) {
    Wrappers out;
    struct File { std::string name; std::filesystem::path path; };
    std::vector<File> files;
    auto add = [&](const std::filesystem::path& file) {
        const auto name = wrapper_path(file);
        for (const auto& existing : files) {
            std::error_code ec;
            if (name == existing.name || std::filesystem::equivalent(file, existing.path, ec)) return;
        }
        files.push_back({name, file});
    };
    for (unsigned i = 0; i < wrapper_names.size(); ++i) {
        // DLSS ships with the game and can remain after a frame-gen package is removed.
        // Only the interposer means Streamline is installed; its other helpers do not.
        if (i == 1) continue;
        bool found = false;
        if (!game.empty()) {
            for (const auto& dir : {game, game / L"Streamline"}) {
                const auto file = dir / wrapper_names[i];
                std::error_code ec;
                if (std::filesystem::is_regular_file(file, ec)) { add(file); found = true; }
            }
        }
        // Windows' own dxgi/opengl32/powrprof modules do not identify a local wrapper.
        if (!system_names[i] && !loaded[i].empty()) {
            add(loaded[i]);
            found = true;
        }
        if (!i) out.streamline = found;
    }
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.name < b.name; });
    // FNV-1a is a change detector, not an authenticity check. Stream the few known files
    // with bounded memory: content catches replacements even when size and time match.
    uint64_t hash = UINT64_C(14695981039346656037);
    auto bytes = [&](const char* data, size_t size) {
        for (size_t i = 0; i < size; ++i) { hash ^= static_cast<unsigned char>(data[i]); hash *= UINT64_C(1099511628211); }
    };
    auto number = [&](uint64_t n) {
        for (unsigned i = 0; i < 8; ++i) { const char b = char(n & 255); bytes(&b, 1); n >>= 8; }
    };
    number(files.size());
    for (const auto& file : files) {
        number(file.name.size()); bytes(file.name.data(), file.name.size());
        std::error_code ec;
        number(std::filesystem::file_size(file.path, ec));
        const auto time = std::filesystem::last_write_time(file.path, ec);
        number(ec ? 0 : uint64_t(time.time_since_epoch().count()));
        std::ifstream in(file.path, std::ios::binary);
        std::array<char, 16384> buffer;
        while (in.read(buffer.data(), buffer.size()) || in.gcount()) bytes(buffer.data(), size_t(in.gcount()));
        number(in.eof() ? 1 : 0); // unreadable files have a stable identity too
    }
    std::ostringstream encoded;
    encoded << "w2-" << std::hex << hash;
    out.identity = encoded.str(); // differs from every old presence-only record, including "m" and "mb"
    return out;
}
} // namespace kui::overlay_guard
