// SPDX-License-Identifier: MIT
// The same PE reader as sigdb, without Windows dependencies so host tests exercise it too.
#pragma once
#include <utility>
#include "../../../core/src/pe_text.h"

namespace hot::code {

struct Identity {
    uintptr_t base;
    uint32_t timestamp, image_size, text_rva;
    size_t text_size;
};

inline bool OriginalText(const uint8_t* file, size_t size, const Identity& live, const uint8_t* loaded,
                         std::vector<uint8_t>& out, std::string& why,
                         bool (*read_pointer)(uintptr_t, uintptr_t*) = nullptr)
{
    constexpr size_t limit = 512ull * 1024 * 1024;
    kiln::pe::FileText ft;
    if (!kiln::pe::parse_file_text(file, size, ft, why)) return false;
    if (ft.timestamp != live.timestamp || ft.size_of_image != live.image_size ||
        ft.text_rva != live.text_rva || ft.text_vsize != live.text_size) {
        why = "the exe file does not match the loaded image";
        return false;
    }
    if (ft.text_vsize > limit) {
        why = "the exe code exceeds 512 MiB";
        return false;
    }
    std::vector<uint8_t> text(ft.text_vsize, 0);
    std::memcpy(text.data(), file + ft.text_raw, ft.file_bytes());
    kiln::pe::relocate_text(text.data(), text.size(), ft, live.base);
    if (!loaded) return why = "no loaded code", false;
    // Matching headers do not prove identical code. Keep the file bytes under known hooks, and the live bytes
    // everywhere else: an in-place mod patch stays visible, and a changed file cannot invent an unrelated match.
    std::vector<kiln::pe::DiffRun> changes;
    const auto differences = kiln::pe::other_differences(text.data(), loaded, text.size(),
        live.base + live.text_rva, live.base, live.image_size, nullptr, 0, false, read_pointer, &changes);
    size_t recorded = 0;
    for (const auto& run : changes) recorded += run.len;
    if (recorded != differences.bytes) {
        why = "too many exe code changes";
        return false;
    }
    for (const auto& run : changes)
        std::memcpy(text.data() + run.rva, loaded + run.rva, run.len);
    out = std::move(text);
    return true;
}

} // namespace hot::code
