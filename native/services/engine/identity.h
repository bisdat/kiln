// SPDX-License-Identifier: MIT
// Exact-image profile selection (portable; the DLL supplies the facts it reads from the loaded image and the core).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "include/kiln_engine_layout.h"
#include "services/kiln_engine.h"

namespace kiln::engine {

// What the running process is. sha256 = the core's hash of the exe file (KilnBuildInfo.exe_sha256), lower-case hex.
struct ImageFacts {
    bool headers = false;        // the loaded image's DOS/PE headers were read
    std::string target;          // exe file name, e.g. "enshrouded.exe"
    std::string sha256;
    std::uint32_t timestamp = 0, size_of_image = 0;
    std::uint32_t archive_format = 0; // 3 or 4 from the first four bytes of <exe>.kfc ("KFC3"/"KFC4"), 0 otherwise
    std::uint32_t side = 0;
};

struct Selection {
    std::uint32_t status = KILN_ENGINE_UNIDENTIFIED;
    const Profile* profile = nullptr;
    const char* why = "image not identified";
};

// ShroudForge's exact-image rule: an eligible profile for this target matches when the process KFC format is 4 and its
// image sha256 (a 64-hex string), timestamp and size equal the process's; exactly one normal match is selected.
Selection select(const ImageFacts& image, const Profile* profiles, std::size_t count);

// The 4-byte .kfc header -> 3, 4 or 0 (ShroudForge identify_game_image.cpp:57-65).
std::uint32_t archive_format_of(const unsigned char* header, std::size_t bytes);

// Fill the public record (strings cut to their fields, NUL-terminated).
void describe(const ImageFacts& image, const Selection& selection, KilnEngineIdentity& out);

}  // namespace kiln::engine
