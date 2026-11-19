// SPDX-License-Identifier: MIT
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer),
// modules/game-runtime/native/loaded-resources/select_resource_profile.cpp:5-63 (eligibility, exact image, one match)
// and modules/game-runtime/native/profiles/identify_game_image.cpp:44-67 (TimeDateStamp, SizeOfImage, KFC header).
// Profile structure and validation already ran at generation time (tools/engine/generate.py); beta profiles are
// never ported, so only the normal-match branch applies.
#include "identity.h"

#include <cstring>
#include <string_view>

namespace kiln::engine {
namespace {
bool hex64(std::string_view s) {
    if (s.size() != 64) return false;
    for (const char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
void put(char* dst, std::size_t cap, std::string_view value) {
    const std::size_t n = value.size() < cap - 1 ? value.size() : cap - 1;
    std::memcpy(dst, value.data(), n);
    std::memset(dst + n, 0, cap - n);
}
}  // namespace

std::uint32_t archive_format_of(const unsigned char* header, std::size_t bytes) {
    if (!header || bytes < 4 || header[0] != 'K' || header[1] != 'F' || header[2] != 'C') return 0;
    return header[3] == '3' ? 3u : header[3] == '4' ? 4u : 0u;
}

Selection select(const ImageFacts& image, const Profile* profiles, std::size_t count) {
    Selection result;
    if (!image.headers || image.target.empty() || !image.size_of_image) return result;
    result.status = KILN_ENGINE_UNKNOWN_BUILD;
    result.why = "no exact resource-read profile for this image (sha256, TimeDateStamp, SizeOfImage, KFC4)";
    std::size_t matches = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Profile& p = profiles[i];
        // read_profile (:5-13): schema 1|2, same target, KFC4, read-validated + resourceReadValidated (generator).
        if ((p.schema_version != 1 && p.schema_version != 2) || p.archive_format != 4 || image.target != p.target) continue;
        // :41 and exact_image (:14-24): process KFC4 and an exact sha256/timestamp/size, with a real 64-hex hash.
        if (image.archive_format != 4 || !hex64(image.sha256) || image.sha256 != p.sha256 ||
            image.timestamp != p.timestamp || image.size_of_image != p.size_of_image)
            continue;
        ++matches;
        result.profile = &p;
    }
    if (matches == 1) {
        result.status = KILN_ENGINE_EXACT;
        result.why = "";
    } else {
        result.profile = nullptr;
        if (matches > 1) {
            result.status = KILN_ENGINE_AMBIGUOUS;
            result.why = "more than one exact resource-read profile matches this image";
        }
    }
    return result;
}

void describe(const ImageFacts& image, const Selection& selection, KilnEngineIdentity& out) {
    KilnEngineIdentity r{};
    r.size = sizeof r;
    r.status = selection.status;
    r.side = image.side;
    r.archive_format = image.archive_format;
    r.timestamp = image.timestamp;
    r.size_of_image = image.size_of_image;
    if (selection.profile) {
        r.sections = selection.profile->sections;
        r.schema_version = selection.profile->schema_version;
    }
    put(r.target, sizeof r.target, image.target);
    put(r.sha256, sizeof r.sha256, image.sha256);
    put(r.profile_id, sizeof r.profile_id, selection.profile ? selection.profile->id : "");
    out = r;
}

}  // namespace kiln::engine
